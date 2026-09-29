// mpc_engine() for "Machinedrum Tap": one Machinedrum track (mono) or send (stereo), read from the shared state of the
// "Machinedrum Module" instance in the same MPC process (vst/tap_shared.h), so MPC's own mixer, submixes and send effects
// can process it. A tap has no engine of its own: it only reads. Silence until a Machinedrum Module exists.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#include "tap_shared.h"

extern "C" {
#include "engine.h"
}

namespace {

using mdtap::Shared;

Shared* findShared()
{
	static Shared* s = nullptr;
	static bool tried = false;
	if(s) return s;
	Dl_info di;
	if(!dladdr(reinterpret_cast<void*>(&findShared), &di) || !di.dli_fname) return nullptr;
	std::string dir = di.dli_fname;
	dir = dir.substr(0, dir.find_last_of('/'));
	// The primary is a different plugin file, normally next to this one (/sdcard/vst) or in a plugin folder.
	for(const std::string& path : {dir + "/machinedrum_one.so", std::string("/sdcard/vst/machinedrum_one.so")})
	{
		void* h = dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);	// only if MPC has already loaded it: shares its statics
		if(!h) continue;
		auto fn = reinterpret_cast<mdtap::Shared* (*)()>(dlsym(h, "md_tap_shared"));
		if(fn && (s = fn()) && s->magic == Shared::kMagic) return s;
		s = nullptr;
	}
	(void)tried;
	return nullptr;
}

constexpr int64_t kPeriodUs = 2902;	// 128 frames at 44.1 kHz

struct Tap
{
#ifdef TAP_FX
	std::atomic<int> source{mdtap::kSrcRev};
#else
	std::atomic<int> source{0};
#endif
	std::atomic<int> through{0};
	int counted = 0;	// the source currently registered in Shared (render/host thread only)
	Shared* sh = nullptr;
	int calls = 0;
	// Alignment diagnostics: with /tmp/md-stats-on present, once a second /tmp/md-tap-stats.<pid> gets the counts of calls that
	// found the primary already called this period vs not, and the time-since-primary range of each case.
	int64_t nFirst = 0, nSecond = 0, minFirst = 1 << 30, maxFirst = -(1 << 30), minSecond = 1 << 30, maxSecond = -(1 << 30);
	std::chrono::steady_clock::time_point statT = std::chrono::steady_clock::now();
	void note(bool first, int64_t dt)
	{
		if(access("/tmp/md-stats-on", F_OK) != 0) return;
		(first ? nFirst : nSecond)++;
		auto& lo = first ? minFirst : minSecond; auto& hi = first ? maxFirst : maxSecond;
		lo = std::min(lo, dt); hi = std::max(hi, dt);
		const auto n = std::chrono::steady_clock::now();
		if(n - statT < std::chrono::seconds(1)) return;
		statT = n;
		if(FILE* f = std::fopen(("/tmp/md-tap-stats." + std::to_string(getpid())).c_str(), "a"))
		{
			std::fprintf(f, "source=%d primary_first=%lld (dt %lld..%lld us) primary_second=%lld (dt %lld..%lld us)\n", source.load(), (long long)nFirst, (long long)minFirst, (long long)maxFirst, (long long)nSecond, (long long)minSecond, (long long)maxSecond);
			std::fclose(f);
		}
		nFirst = nSecond = 0; minFirst = minSecond = 1 << 30; maxFirst = maxSecond = -(1 << 30);
	}

	void unregister()
	{
		if(!sh) return;
		if(counted >= mdtap::kSrcTrack1 && counted < mdtap::kSrcTrack1 + mdtap::kTracks) sh->tapped[counted - 1].fetch_sub(1);
		else if(counted == mdtap::kSrcRev) sh->tapsRev.fetch_sub(1);
		else if(counted == mdtap::kSrcDel) sh->tapsDel.fetch_sub(1);
		counted = 0;
	}
	void sync(int want)
	{
		if(!sh || want == counted) return;
		unregister();
		if(want >= mdtap::kSrcTrack1 && want < mdtap::kSrcTrack1 + mdtap::kTracks) sh->tapped[want - 1].fetch_add(1);
		else if(want == mdtap::kSrcRev) sh->tapsRev.fetch_add(1);
		else if(want == mdtap::kSrcDel) sh->tapsDel.fetch_add(1);
		counted = want;
	}
};

void* eCreate(const char*) { return new Tap(); }
void eDestroy(void* p) { auto* t = static_cast<Tap*>(p); t->unregister(); delete t; }
void eMidi(void*, const uint8_t*, int) {}
void eSet(void* p, const char* key, const char* val)
{
	if(!std::strcmp(key, "source")) static_cast<Tap*>(p)->source.store(std::clamp(std::atoi(val), 0, mdtap::kSources - 1));
	else if(!std::strcmp(key, "through")) static_cast<Tap*>(p)->through.store(std::atoi(val) != 0);
}
int eGet(void* p, const char* key, char* buf, int len)
{
	if(!std::strcmp(key, "source")) return std::snprintf(buf, static_cast<size_t>(len), "%d", static_cast<Tap*>(p)->source.load());
	if(!std::strcmp(key, "through")) return std::snprintf(buf, static_cast<size_t>(len), "%d", static_cast<Tap*>(p)->through.load());
	return 0;
}

void fill(Tap* t, int16_t* out, int frames)
{
	std::memset(out, 0, sizeof(int16_t) * static_cast<size_t>(frames) * 2);
	if(frames != mdtap::kFrames) return;
	if(!t->sh)
	{
		if(++t->calls % 64 != 1) return;	// look for the primary about every 190 ms
		t->sh = findShared();
		if(!t->sh) return;
	}
	Shared& s = *t->sh;
	if(!s.owner.load(std::memory_order_acquire)) { t->unregister(); return; }
	const int src = t->source.load(std::memory_order_relaxed);
	t->sync(src);
	// Take the block the primary's host takes in this same period: if it has already been called this period (within half a
	// period) that is the one it just took (hostRead - 1), else the one it is about to take (hostRead). MPC calls the instances
	// in no fixed order, so this is decided per call from the primary's last call time.
	const uint32_t hr = s.hostRead.load(std::memory_order_acquire);
	const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const int64_t dt = nowUs - s.hostCallUs.load(std::memory_order_acquire);
	const bool primaryFirst = dt >= 0 && dt < kPeriodUs / 2;
	const uint32_t w = s.written.load(std::memory_order_acquire);
	const uint32_t r = primaryFirst ? hr - 1 : hr;
	t->note(primaryFirst, dt);
	if(src == mdtap::kSrcOff || static_cast<int32_t>(w - r) <= 0 || static_cast<int32_t>(w - r) >= mdtap::kSlots - 1) return;
	const auto& blk = s.data[r % mdtap::kSlots];
	if(src >= mdtap::kSrcTrack1 && src < mdtap::kSrcTrack1 + mdtap::kTracks)
	{
		const int16_t* m = blk[src - 1];
		for(int i = 0; i < frames; ++i) out[2 * i] = out[2 * i + 1] = m[i];
	}
	else
	{
		const int base = src == mdtap::kSrcRev ? mdtap::kPlaneRev : mdtap::kPlaneDel;
		for(int i = 0; i < frames; ++i) { out[2 * i] = blk[base][i]; out[2 * i + 1] = blk[base + 1][i]; }
	}
}

void eRender(void* p, int16_t* out, int frames) { fill(static_cast<Tap*>(p), out, frames); }

// The effect build ("Machinedrum Tap FX", on an MPC return/FX track): its output is the tapped source, plus the input it
// was given when "through" is on, so MPC's own insert effects after it process the Machinedrum's reverb or delay send.
void eProcess(void* p, const int16_t* in, int16_t* out, int frames)
{
	auto* t = static_cast<Tap*>(p);
	fill(t, out, frames);
	if(t->through.load(std::memory_order_relaxed))
		for(int i = 0; i < frames * 2; ++i) out[i] = static_cast<int16_t>(std::clamp(out[i] + in[i], -32768, 32767));
}

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, eProcess};

}

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
