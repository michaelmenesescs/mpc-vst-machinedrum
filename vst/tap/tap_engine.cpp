// mpc_engine() for "Machinedrum Tap": one Machinedrum track (mono) or send (stereo), read from the shared state of the
// "Machinedrum Module" instance in the same MPC process (vst/tap_shared.h), so MPC's own mixer, submixes and send effects
// can process it. A tap has no engine of its own: it only reads. Silence until a Machinedrum Module exists.
#include <algorithm>
#include <atomic>
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

struct Tap
{
	std::atomic<int> source{0};
	int counted = 0;	// the source currently registered in Shared (render/host thread only)
	uint32_t r = 0;
	bool aligned = false;
	Shared* sh = nullptr;
	int calls = 0;

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
}
int eGet(void* p, const char* key, char* buf, int len)
{
	if(!std::strcmp(key, "source")) return std::snprintf(buf, static_cast<size_t>(len), "%d", static_cast<Tap*>(p)->source.load());
	return 0;
}

void eRender(void* p, int16_t* out, int frames)
{
	auto* t = static_cast<Tap*>(p);
	std::memset(out, 0, sizeof(int16_t) * static_cast<size_t>(frames) * 2);
	if(frames != mdtap::kFrames) return;
	if(!t->sh)
	{
		if(++t->calls % 64 != 1) return;	// look for the primary about every 190 ms
		t->sh = findShared();
		if(!t->sh) return;
	}
	Shared& s = *t->sh;
	if(!s.owner.load(std::memory_order_acquire)) { t->aligned = false; t->unregister(); return; }
	const int src = t->source.load(std::memory_order_relaxed);
	t->sync(src);
	// Follow the primary's host reads: this call takes the block the primary's host is at (a few ms of offset between
	// instances is inherent: MPC runs them on different threads in no fixed order).
	const uint32_t hr = s.hostRead.load(std::memory_order_acquire);
	if(!t->aligned || static_cast<int32_t>(t->r - hr) < -2 || static_cast<int32_t>(t->r - hr) > 2) { t->r = hr; t->aligned = true; }
	const uint32_t w = s.written.load(std::memory_order_acquire);
	const uint32_t r = t->r++;
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

const mpc_engine_t kEngine = {eCreate, eDestroy, eMidi, eSet, eGet, eRender, nullptr};

}

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }
