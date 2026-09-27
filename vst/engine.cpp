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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "Engine.h"
#include "Firmware.h"

extern "C" {
#include "engine.h"
}

namespace {

using md::engine::Engine;

constexpr int kFrames = 128;					// the host's block size
constexpr int kInner = kFrames / Engine::kBlock;	// 32-sample engine blocks per host block
constexpr int kRing = 4, kAhead = 2;
constexpr int kTracks = Engine::kTracks;
constexpr int kBaseNote = 36;					// MPC/GM kick; note 36 = track 0, 37 = track 1, ...

// slot layout: per-track (machine, level, pan) x 16, then two globals
constexpr int kSlotTrack = 0, kSlotsPerTrack = 3;	// machine, level, pan
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
	if(std::sscanf(key, "track%d_level%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 1;
	if(std::sscanf(key, "track%d_pan%n", &t, &consumed) == 1 && key[consumed] == '\0' && t >= 0 && t < kTracks)
		return kSlotTrack + t * kSlotsPerTrack + 2;
	return -1;
}

struct NoteEv { uint8_t track; uint8_t velocity; };

struct Inst
{
	std::string osPath;
	std::atomic<int> param[kNumSlots];
	std::atomic<bool> stop{false}, ready{false};
	std::atomic<uint32_t> underruns{0}, blocks{0};

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
		for(int t = 0; t < kTracks; ++t)
		{
			param[kSlotTrack + t * kSlotsPerTrack + 1].store(100);	// level
			param[kSlotTrack + t * kSlotsPerTrack + 2].store(64);	// pan (centre)
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
		// Per-track FX defaults not yet exposed as VST params (V1): FLTW open, EQ flat, matching
		// tools/mdrender/mdrender.cpp's demo kit. Without these, FLTW's raw default (0) leaves the track's
		// filter effectively closed - most machines would render near-silent, not just "unfiltered".
		for(int t = 0; t < kTracks; ++t)
		{
			h.setParam(t, 12, 0);	// FLTF
			h.setParam(t, 13, 127);	// FLTW: fully open
			h.setParam(t, 10, 64);	// EQF
			h.setParam(t, 11, 64);	// EQG: flat
		}

		int appliedTempo = -1, appliedMaxVoices = -1;
		int appliedMachine[kTracks], appliedLevel[kTracks], appliedPan[kTracks];
		for(int t = 0; t < kTracks; ++t) appliedMachine[t] = appliedLevel[t] = appliedPan[t] = -1;

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
				const int lv = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 1].load(std::memory_order_relaxed), 0, 127);
				// VOL (param 17, read by the mixer's own gain formula) - not setLevel(), a separate kit LEV
				// knob (HostModel.h) that also gates level but isn't what mdrender.cpp's working demo kit uses.
				if(lv != appliedLevel[t]) { appliedLevel[t] = lv; h.setParam(t, 17, lv); }
				const int pan = std::clamp(param[kSlotTrack + t * kSlotsPerTrack + 2].load(std::memory_order_relaxed), 0, 127);
				if(pan != appliedPan[t]) { appliedPan[t] = pan; h.setParam(t, 18, pan); }
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
