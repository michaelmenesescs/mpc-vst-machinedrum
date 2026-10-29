// md-firsthit: what a machine's first hit costs, and that the JIT warm-up (EngineT::warmUpVoices) changes no sample.
// Two engines play the same script: every warmable machine in turn on track (i % 16), triggered, then 24 blocks. One was
// warmed first, the other is cold (its JIT compiles each machine's code at its first hit). Prints, per machine, the
// slowest block after its hit in each engine, and whether the two engines' output (main, sends, individual outputs and
// every track) is identical sample for sample. Idea and method from c0remusic/MModule's mdDrumsFirstHitTest (GPL-3.0).
// usage: md-firsthit <OS.syx> [ROM_SAMPLES.bin]   MD_GROUPS=n (default 3, as the plugin)  MD_WARM_BLOCKS=n (default 4)
//   MD_FH_BLOCKS=n (blocks after each hit, default 24)  MD_FH_STAGES=1 (the warm engine's stage timing per machine)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "MdEngine.h"
#include "Firmware.h"

using Clock = std::chrono::steady_clock;
using Eng = md::engine::ParallelEngine;

static bool loadRom(Eng& _e, const char* _path)
{
	FILE* rf = fopen(_path, "rb");
	if(!rf) return false;
	char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
	if(fread(magic, 1, 4, rf) == 4 && !memcmp(magic, "MDS1", 4))
		while(fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
		{
			words.resize(head[1]);
			if(fread(words.data(), 4, head[1], rf) != head[1]) break;
			_e.voices().writeP(head[0], words.data(), words.size());
		}
	fclose(rf);
	return true;
}

int main(int argc, char** argv)
{
	if(argc < 2) { fprintf(stderr, "usage: md-firsthit <OS.syx> [ROM_SAMPLES.bin]\n"); return 2; }
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	const int groups = getenv("MD_GROUPS") ? atoi(getenv("MD_GROUPS")) : 3;
	const int warmBlocks = getenv("MD_WARM_BLOCKS") ? atoi(getenv("MD_WARM_BLOCKS")) : 4;
	const int hitBlocks = getenv("MD_FH_BLOCKS") ? atoi(getenv("MD_FH_BLOCKS")) : 24;	// blocks played after each hit
	Eng cold(fwv, std::vector<uint8_t>(c.sections.at(0).data), groups);
	Eng warm(fwv, std::vector<uint8_t>(c.sections.at(0).data), groups);
	const bool rom = argc > 2 && loadRom(cold, argv[2]) && loadRom(warm, argv[2]);
	const auto machines = warm.warmableMachines(rom);
	const auto t0 = Clock::now();
	const int n = warm.warmUpVoices(machines, warmBlocks);
	printf("warm-up: %d machines, %d groups, %d blocks each, %.0f ms\n", n, groups, warmBlocks,
		std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
	if(n < 0) { printf("warm-up fault: %s\n", warm.fault().c_str()); return 1; }

	Eng::Output oc, ow;
	auto same = [](const Eng::Output& _a, const Eng::Output& _b)
	{
		return _a.mix.main == _b.mix.main && _a.mix.frame == _b.mix.frame && _a.mix.rev == _b.mix.rev && _a.mix.del == _b.mix.del && _a.tracks == _b.tracks;
	};
	auto timed = [](Eng& _e, Eng::Output& _o) { const auto a = Clock::now(); if(!_e.render(_o)) { printf("fault: %s\n", _e.fault().c_str()); exit(1); } return std::chrono::duration<double, std::micro>(Clock::now() - a).count(); };
	// warm-up's own blocks are not in the output: both engines start the same
	if(getenv("MD_FH_STAGES")) { warm.timingOn = true; warm.voices().timingOn = true; }
	long diffBlocks = 0, blocks = 0;
	for(int b = 0; b < 20; ++b) { timed(cold, oc); timed(warm, ow); diffBlocks += !same(oc, ow); ++blocks; }
	double sumCold = 0, sumWarm = 0, worstCold = 0, worstWarm = 0;
	int overCold = 0, overWarm = 0;	// first hits whose slowest block took longer than a 128-frame period (2902 us)
	for(size_t i = 0; i < machines.size(); ++i)
	{
		const int t = static_cast<int>(i % 16);
		const auto* m = cold.os().machine(machines[i]);
		for(Eng* e : {&cold, &warm}) { e->host().setMachine(t, machines[i]); e->host().trigger(t, 100); }
		double wc = 0, ww = 0;
		bool eq = true;
		for(int b = 0; b < hitBlocks; ++b)
		{
			wc = std::max(wc, timed(cold, oc));
			ww = std::max(ww, timed(warm, ow));
			const bool s = same(oc, ow);
			eq &= s; diffBlocks += !s; ++blocks;
		}
		sumCold += wc; sumWarm += ww; worstCold = std::max(worstCold, wc); worstWarm = std::max(worstWarm, ww);
		overCold += wc > 2902; overWarm += ww > 2902;
		if(getenv("MD_FH_STAGES"))	// where the warm engine's time went over this machine's 24 blocks
		{
			auto& h = warm.host();
			printf("    warm stages: tick %.0f us, voice DSP %.0f us (groups", h.tickUs, h.dspUs);
			for(double g : warm.voices().groupUs) printf(" %.0f", g);
			printf("), fx %.0f us, mix %.0f us\n", warm.fxUs, warm.mixUs);
			h.tickUs = h.dspUs = warm.fxUs = warm.mixUs = 0;
			for(auto& g : warm.voices().groupUs) g = 0;
		}
		printf("HIT %3d %-8s cold %7.0f us  warm %6.0f us  %s\n", m->id, m->name.c_str(), wc, ww, eq ? "identical" : "DIFFERENT");
	}
	// and a tail: everything decays the same
	double tailCold = 0, tailWarm = 0;
	for(int b = 0; b < 2000; ++b) { tailCold = std::max(tailCold, timed(cold, oc)); tailWarm = std::max(tailWarm, timed(warm, ow)); diffBlocks += !same(oc, ow); ++blocks; }
	printf("tail (2000 blocks): slowest block cold %.0f us, warm %.0f us\n", tailCold, tailWarm);
	printf("first hits: %zu machines; slowest block after a hit: cold mean %.0f us, max %.0f us, %d over 2902 us; warm mean %.0f us, max %.0f us, %d over\n",
		machines.size(), sumCold / machines.size(), worstCold, overCold, sumWarm / machines.size(), worstWarm, overWarm);
	printf("output: %ld of %ld blocks differ (%s)\n", diffBlocks, blocks, diffBlocks ? "NOT identical" : "identical");
	return diffBlocks ? 1 : 0;
}
