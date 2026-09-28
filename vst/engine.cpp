// mpc_engine() (mpc-vst-plugins wrapper/engine.h) for Machinedrum One: all 16 voices from one instance, a
// MIDI note-number drum map (note - kBaseNote = track, clamped 0-15; velocity -> trigger velocity). See
// HANDOFF.md, "Design goal: all voices in one plugin instance".
//
// The engine runs on its own persistent thread (created once at create(), not per-block - see HANDOFF.md's
// note on why per-block thread spawn is the wrong design), rendering 128-frame blocks ahead of the host into
// a small ring, same architecture as mpc-vst-monomodule's DSP thread. render() (the host's audio callback)
// only copies a finished block out, or outputs silence when the DSP thread is behind (counted, never
// waited for).
//
// Parameters (V1, kept deliberately simple): per track (0-15), machine (a raw OS machine id - not a named
// option list, since the id table is decoded from the user's own firmware at runtime, not something this
// repo can commit; see docs/FIRMWARE.md), level and pan. Plus two globals: tempo (for LFO/E12 timing) and
// max_voices (HostModel::setMaxActiveVoices - a voice cap safety valve, see HANDOFF.md "adjustable voice
// cap"). Per-track FX (AMD/EQ/filter/SRR/DIST) and per-track LFOs are not yet exposed - a natural next
// increment once this basic version is verified on the device.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "Engine.h"
#include "Firmware.h"

extern "C" {
#include "engine.h"
}

namespace {

using md::engine::Engine;

// One engine per core, never the UI core (MPC's main thread lives on cpu0): take the least busy of
// cores 1..N-1 (sampled from /proc/stat over 100 ms). Same approach as mpc-vst-monomodule's chooseCore().
int chooseCore()
{
	auto sample = [](unsigned long long* busy, unsigned long long* total, int n)
	{
		std::FILE* f = std::fopen("/proc/stat", "r");
		if(!f) return;
		char line[256];
		while(std::fgets(line, sizeof line, f))
		{
			int c; unsigned long long u, ni, s, id, io, ir, so, st;
			if(std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &ir, &so, &st) == 9 && c >= 0 && c < n)
			{
				busy[c] = u + ni + s + ir + so + st;
				total[c] = busy[c] + id + io;
			}
		}
		std::fclose(f);
	};
	const int n = static_cast<int>(std::min<long>(sysconf(_SC_NPROCESSORS_ONLN), 8));
	if(n < 2) return -1;
	unsigned long long b0[8] = {}, t0[8] = {}, b1[8] = {}, t1[8] = {};
	sample(b0, t0, n);
	struct timespec ts{0, 100000000};
	nanosleep(&ts, nullptr);
	sample(b1, t1, n);
	int best = -1; double bestLoad = 2;
	for(int c = 1; c < n; ++c)
	{
		const double dt = static_cast<double>(t1[c] - t0[c]);
		const double load = dt > 0 ? static_cast<double>(b1[c] - b0[c]) / dt : 0;
		if(load < bestLoad) { bestLoad = load; best = c; }
	}
	return best;
}

constexpr int kFrames = 128;					// the host's block size
constexpr int kInner = kFrames / Engine::kBlock;	// 32-sample engine blocks per host block
constexpr int kRing = 4, kAhead = 2;
constexpr int kTracks = Engine::kTracks;
constexpr int kBaseNote = 36;					// MPC/GM kick; note 36 = track 0, 37 = track 1, ...

// slot layout, per track: 0=machine, 1=vol (HostModel raw param 17), 2=pan (param 18), 3.. = the
// AMP/EFX page's 8 params (raw 8-15, real hardware page order - see gen_params.py's FX_PARAMS), then
// the ROUTE page's 6 not-otherwise-exposed params (DIST raw 16, DEL/REV/LFOS/LFOD/LFOM raw 19-23 -
// VOL/PAN raw 17/18 are ROUTE-page params too on real hardware, but reuse the existing vol/pan keys
// above rather than duplicating them - see HANDOFF.md, "found the third per-track page: ROUTE").
// Not yet exposed: SYN1-8 - see gen_params.py's docstring.
constexpr const char* kFxKeys[] = {"amd", "amf", "eqf", "eqg", "fltf", "fltw", "fltq", "srr"};
constexpr int kNumFx = sizeof(kFxKeys) / sizeof(kFxKeys[0]);
constexpr int kFxRawParamBase = 8;	// HostModel raw param index of kFxKeys[0] ("amd")

constexpr const char* kRouteKeys[] = {"dist", "del", "rev", "lfos", "lfod", "lfom"};
constexpr int kRouteRawParam[] = {16, 19, 20, 21, 22, 23};	// not contiguous (17/18 are vol/pan)
constexpr int kNumRoute = sizeof(kRouteKeys) / sizeof(kRouteKeys[0]);
static_assert(sizeof(kRouteRawParam) / sizeof(kRouteRawParam[0]) == kNumRoute);

constexpr int kSlotTrack = 0, kSlotsPerTrack = 3 + kNumFx + kNumRoute;	// machine, vol, pan, FX, ROUTE
constexpr int kSlotTempo = kSlotTrack + kTracks * kSlotsPerTrack;
constexpr int kSlotMaxVoices = kSlotTempo + 1;
constexpr int kNumSlots = kSlotMaxVoices + 1;

int slotOf(const char* key)
{
	if(!std::strcmp(key, "tempo")) return kSlotTempo;
	if(!std::strcmp(key, "max_voices")) return kSlotMaxVoices;
	int t = -1, consumed = 0;
	if(std::sscanf(key, "track%d_machine%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 0;
	if(std::sscanf(key, "track%d_vol%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 1;
	if(std::sscanf(key, "track%d_pan%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 2;
	if(std::sscanf(key, "track%d_%n", &t, &consumed) == 1 && t >= 0 && t < kTracks)
	{
		const char* fxKey = key + consumed;
		for(int i = 0; i < kNumFx; ++i)
			if(!std::strcmp(fxKey, kFxKeys[i]))
				return kSlotTrack + t * kSlotsPerTrack + 3 + i;
		for(int i = 0; i < kNumRoute; ++i)
			if(!std::strcmp(fxKey, kRouteKeys[i]))
				return kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i;
	}
	return -1;
}

struct NoteEv { uint8_t track; uint8_t velocity; };

struct Inst
{
	std::string osPath;
	std::atomic<int> param[kNumSlots];
	std::atomic<bool> stop{false}, ready{false};
	std::atomic<uint32_t> underruns{0}, blocks{0};
	std::atomic<int> core{-1};

	NoteEv notes[256];
	std::atomic<uint32_t> nWrite{0}, nRead{0};

	alignas(64) int16_t ring[kRing][kFrames * 2];
	std::atomic<uint32_t> rWrite{0}, rRead{0};
	std::thread th;

	Inst()
	{
		for(auto& p : param) p.store(0);
		param[kSlotTempo].store(120);
		param[kSlotMaxVoices].store(kTracks);
		// Matches gen_params.py's declared defaults: the host normally pushes these via set_param right
		// after create(), but this is what plays if render() is called before that (or from a host that
		// doesn't restore params on creation).
		constexpr int kFxDefaults[kNumFx] = {0, 0, 64, 64, 0, 127, 0, 0};	// amd amf eqf eqg fltf fltw fltq srr
		constexpr int kRouteDefaults[kNumRoute] = {0, 0, 0, 0, 0, 0};	// dist del rev lfos lfod lfom
		for(int t = 0; t < kTracks; ++t)
		{
			param[kSlotTrack + t * kSlotsPerTrack + 1].store(100);	// vol
			param[kSlotTrack + t * kSlotsPerTrack + 2].store(64);	// pan (centre)
			for(int i = 0; i < kNumFx; ++i)
				param[kSlotTrack + t * kSlotsPerTrack + 3 + i].store(kFxDefaults[i]);
			for(int i = 0; i < kNumRoute; ++i)
				param[kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i].store(kRouteDefaults[i]);
		}
	}

	void run();
};

void Inst::run()
{
	try
	{
		const auto fwv = md::fw::loadFirmware(osPath);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(osPath)));
		Engine eng(fwv, std::move(c.sections.at(0).data));
		auto& h = eng.host();

		// Real-time priority, above MPC's own AudioWorkers (SCHED_RR 20) - only once booted, so the boot
		// itself doesn't hog the CPU at real-time priority. Without this the render thread is a plain
		// SCHED_OTHER thread that can get starved under system load, heard as choppy/dropped audio (a
		// plain background thread was the whole design point of this architecture - see the top-of-file
		// comment - but it still needs real-time scheduling to actually keep up, same as
		// mpc-vst-monomodule's own DSP thread).
		{
			int prio = 30;
			if(const char* e = std::getenv("MD_FIFO")) prio = std::atoi(e);
			if(prio > 0)
			{
				sched_param sp{};
				sp.sched_priority = prio;
				pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
			}
			const int c = std::getenv("MD_CPU") ? std::atoi(std::getenv("MD_CPU")) : chooseCore();
			if(c >= 0)
			{
				cpu_set_t s;
				CPU_ZERO(&s);
				CPU_SET(c, &s);
				sched_setaffinity(0, sizeof s, &s);
			}
			core.store(c, std::memory_order_relaxed);
		}

		int appliedTempo = -1, appliedMaxVoices = -1;
		int appliedMachine[kTracks], appliedVol[kTracks], appliedPan[kTracks];
		int appliedFx[kTracks][kNumFx];
		int appliedRoute[kTracks][kNumRoute];
		for(int t = 0; t < kTracks; ++t)
		{
			appliedMachine[t] = appliedVol[t] = appliedPan[t] = -1;
			for(int i = 0; i < kNumFx; ++i) appliedFx[t][i] = -1;
			for(int i = 0; i < kNumRoute; ++i) appliedRoute[t][i] = -1;
		}

		Engine::Output out;
		ready.store(true);

		while(!stop.load(std::memory_order_acquire))
		{
			const uint32_t w = rWrite.load(std::memory_order_relaxed);
			if(int32_t(w - rRead.load(std::memory_order_acquire)) >= kAhead)
			{
				struct timespec ts{0, 400000};
				nanosleep(&ts, nullptr);
				continue;
			}

			const int tempo = std::clamp(param[kSlotTempo].load(std::memory_order_relaxed), 30, 300);
			if(tempo != appliedTempo) { appliedTempo = tempo; h.setTempo(tempo); }
			const int maxVoices = std::clamp(param[kSlotMaxVoices].load(std::memory_order_relaxed), 1, kTracks);
			if(maxVoices != appliedMaxVoices) { appliedMaxVoices = maxVoices; h.setMaxActiveVoices(maxVoices); }
			for(int t = 0; t < kTracks; ++t)
			{
				const int m = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 0].load(std::memory_order_relaxed), 0, 191);
				if(m != appliedMachine[t]) { appliedMachine[t] = m; h.setMachine(t, static_cast<uint8_t>(m)); }
				const int vol = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 1].load(std::memory_order_relaxed), 0, 127);
				// VOL (param 17, read by the mixer's own gain formula) - not setLevel(), a separate kit LEV
				// knob (HostModel.h) that also gates level but isn't what mdrender.cpp's working demo kit uses.
				if(vol != appliedVol[t]) { appliedVol[t] = vol; h.setParam(t, 17, vol); }
				const int pan = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 2].load(std::memory_order_relaxed), 0, 127);
				if(pan != appliedPan[t]) { appliedPan[t] = pan; h.setParam(t, 18, pan); }
				for(int i = 0; i < kNumFx; ++i)
				{
					const int v = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 3 + i].load(std::memory_order_relaxed), 0, 127);
					if(v != appliedFx[t][i]) { appliedFx[t][i] = v; h.setParam(t, kFxRawParamBase + i, v); }
				}
				for(int i = 0; i < kNumRoute; ++i)
				{
					const int v = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 3 + kNumFx + i].load(std::memory_order_relaxed), 0, 127);
					if(v != appliedRoute[t][i]) { appliedRoute[t][i] = v; h.setParam(t, kRouteRawParam[i], v); }
				}
			}

			uint32_t r = nRead.load(std::memory_order_relaxed);
			const uint32_t nw = nWrite.load(std::memory_order_acquire);
			for(; r != nw; ++r)
			{
				const NoteEv e = notes[r & 255];
				h.trigger(e.track, e.velocity);
			}
			nRead.store(r, std::memory_order_release);

			int16_t* dst = ring[w % kRing];
			for(int i = 0; i < kInner; ++i)
			{
				if(!eng.render(out))
				{
					std::fprintf(stderr, "[machinedrum] DSP fault: %s\n", eng.fault().c_str());
					return;
				}
				for(int f = 0; f < Engine::kBlock; ++f)
				{
					const int idx = i * Engine::kBlock + f;
					// 24-bit -> 16-bit
					dst[2 * idx + 0] = static_cast<int16_t>(std::clamp(out.mix.main[f][0] >> 8, -32768, 32767));
					dst[2 * idx + 1] = static_cast<int16_t>(std::clamp(out.mix.main[f][1] >> 8, -32768, 32767));
				}
			}
			rWrite.store(w + 1, std::memory_order_release);
			blocks.fetch_add(1, std::memory_order_relaxed);
		}
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "[machinedrum] %s\n", e.what());
	}
}

void* eCreate(const char* dataDir)
{
	auto* in = new Inst();
	if(const char* p = std::getenv("MD_OS")) in->osPath = p;
	else in->osPath = std::string(dataDir && *dataDir ? dataDir : ".") + "/Elektron_SPS1-1UW_OS1.63.syx";
	in->th = std::thread([in] { in->run(); });
	return in;
}

void eDestroy(void* p)
{
	auto* in = static_cast<Inst*>(p);
	in->stop.store(true, std::memory_order_release);
	if(in->th.joinable()) in->th.join();
	delete in;
}

void eMidi(void* p, const uint8_t* msg, int len)
{
	auto* in = static_cast<Inst*>(p);
	if(len < 3) return;
	const uint8_t status = msg[0] & 0xf0;
	if(status == 0x90 && msg[2] > 0)	// note on
	{
		const int track = static_cast<int>(msg[1]) - kBaseNote;
		if(track < 0 || track >= kTracks) return;
		const uint32_t w = in->nWrite.load(std::memory_order_relaxed);
		in->notes[w & 255] = {static_cast<uint8_t>(track), msg[2]};
		in->nWrite.store(w + 1, std::memory_order_release);
	}
}

void eSet(void* p, const char* key, const char* val)
{
	auto* in = static_cast<Inst*>(p);
	const int slot = slotOf(key);
	if(slot < 0) return;
	in->param[slot].store(std::atoi(val), std::memory_order_relaxed);
}

int eGet(void* p, const char* key, char* buf, int bufLen)
{
	auto* in = static_cast<Inst*>(p);
	if(!std::strcmp(key, "ready"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->ready.load(std::memory_order_acquire) ? 1 : 0) > 0;
	if(!std::strcmp(key, "underruns"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%u", in->underruns.load(std::memory_order_relaxed)) > 0;
	if(!std::strcmp(key, "blocks"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%u", in->blocks.load(std::memory_order_relaxed)) > 0;
	if(!std::strcmp(key, "core"))
		return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->core.load(std::memory_order_relaxed)) > 0;
	const int slot = slotOf(key);
	if(slot < 0) return 0;
	return std::snprintf(buf, static_cast<size_t>(bufLen), "%d", in->param[slot].load(std::memory_order_relaxed)) > 0;
}

void eRender(void* p, int16_t* out, int frames)
{
	auto* in = static_cast<Inst*>(p);
	if(!in->ready.load(std::memory_order_acquire) || frames != kFrames)
	{
		std::memset(out, 0, sizeof(int16_t) * static_cast<size_t>(frames) * 2);
		return;
	}
	const uint32_t r = in->rRead.load(std::memory_order_relaxed);
	if(int32_t(in->rWrite.load(std::memory_order_acquire) - r) > 0)
	{
		std::memcpy(out, in->ring[r % kRing], sizeof(int16_t) * kFrames * 2);
		in->rRead.store(r + 1, std::memory_order_release);
	}
	else
	{
		std::memset(out, 0, sizeof(int16_t) * kFrames * 2);
		in->underruns.fetch_add(1, std::memory_order_relaxed);
	}
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, nullptr};

}

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
