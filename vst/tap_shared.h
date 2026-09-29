// State shared between "Machinedrum Module" (the primary: owns the engine) and "Machinedrum Tap" instances, which live in
// the same MPC process. The tap library finds it through md_tap_shared(), exported by machinedrum_one.so (dlopen RTLD_NOLOAD).
// One track or send per tap, mono for tracks, so MPC's own mixer, submixes and send effects can process each one.
#pragma once
#include <atomic>
#include <cstdint>

namespace mdtap
{
	constexpr int kTracks = 16, kFrames = 128, kSlots = 8;
	// planes: 0-15 the tracks (mono, post effects and VOL), 16/17 reverb send L/R, 18/19 delay send L/R
	constexpr int kPlanes = kTracks + 4, kPlaneRev = 16, kPlaneDel = 18;
	// tap sources (the tap's "source" param): 0 off, 1-16 track, 17 reverb send, 18 delay send
	constexpr int kSrcOff = 0, kSrcTrack1 = 1, kSrcRev = 17, kSrcDel = 18, kSources = 19;

	struct Shared
	{
		static constexpr uint32_t kMagic = 0x4d445450;
		uint32_t magic = kMagic;
		std::atomic<const void*> owner{nullptr};	// the primary that publishes (the first one created)
		std::atomic<uint32_t> written{0};			// blocks published
		std::atomic<uint32_t> hostRead{0};			// blocks the primary's host has taken
		std::atomic<int> tapped[kTracks];			// taps reading each track: that track leaves the primary's main mix
		std::atomic<int> tapsRev{0}, tapsDel{0};
		alignas(64) int16_t data[kSlots][kPlanes][kFrames];
	};
}

extern "C" mdtap::Shared* md_tap_shared(void);
