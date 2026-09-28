// mdfxtest: engine/TrackFx (native C++) against the mixer DSP's own per-track code (MixerRef), word for word.
// usage: mdfxtest OS.syx STAGE [TRIALS] [BLOCKS]   STAGE: amd | all
// Each trial: random parameters (with extremes), a random mix of saw, noise, sine, silence and full-scale
// input; compares the stage's output buffer and the track's whole state block after every block.
#include "MixerRef.h"
#include "../../engine/TrackFx.h"
#include "../../engine/Dsp56.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

using namespace md;

int main(int argc, char** argv)
{
	if(argc < 3) { std::fprintf(stderr, "usage: mdfxtest OS.syx amd|all [TRIALS] [BLOCKS]\n"); return 2; }
	const auto fwv = fw::loadFirmware(argv[1]);
	const bool all = !std::strcmp(argv[2], "all");
	struct StageDef { const char* name; engine::TrackFx::Stop stop; uint32_t pc, xFrom, xTo; };
	static const StageDef stages[] = {
		{"amd", engine::TrackFx::Stop::AfterAmd, 0xd2, 0xb, 0x2b},
		{"eq", engine::TrackFx::Stop::AfterEq, 0x134, 0x8, 0x2b},
		{"f1", engine::TrackFx::Stop::AfterFilter1, 0x1a0, 0x6, 0x29},
		{"f2", engine::TrackFx::Stop::AfterFilter2, 0x21f, 0x0, 0x29},
		{"srr", engine::TrackFx::Stop::AfterSrr, 0x234, 0x0, 0x29},
		{"all", engine::TrackFx::Stop::None, 0x25e, 0, 0},
	};
	const StageDef* sd = nullptr;
	for(const auto& d : stages) if(!std::strcmp(argv[2], d.name)) sd = &d;
	if(!sd) { std::fprintf(stderr, "unknown stage\n"); return 2; }
	const int trials = argc > 3 ? std::atoi(argv[3]) : 50;
	const int blocks = argc > 4 ? std::atoi(argv[4]) : 40;
	const bool walk = argc > 5 && !std::strcmp(argv[5], "walk");	// parameters move every block
	mixref::MixerRef ref(fwv);
	engine::TrackFx::Tables tables(fwv);

	// The tables: check the generated sine against the DSP's own
	int sineBad = 0;
	for(uint32_t a = 0x148000; a < 0x150000; ++a)
		if(engine::d56::u24(tables[a]) != ref.peekX(a)) ++sineBad;
	std::printf("sine table: %d/32768 words differ\n", sineBad);

	engine::TrackFx fx(tables);
	fx.stopAfter = sd->stop;
	const uint32_t stopAt = sd->pc, xFrom = sd->xFrom, xTo = sd->xTo;
	uint32_t seed = 12345;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	long bad = 0, total = 0;
	for(int t = 0; t < trials; ++t)
	{
		const int track = static_cast<int>(rnd() % 16);
		engine::TrackFx::State st;
		fx.resetScratch();
		// Start from the DSP's current state for this track (as left by earlier trials)
		for(int k = 0; k < 64; ++k) st.y[k] = engine::d56::sx24(ref.peekY(0x200 + 0x40 * static_cast<uint32_t>(track) + static_cast<uint32_t>(k)));
		uint32_t p[9];
		for(auto& v : p)
		{
			const uint32_t r = rnd() % 10;
			v = (r == 0 ? 0 : r == 1 ? 127 : rnd() % 128) << 7;
		}
		ref.setFx(track, p);
		engine::TrackFx::setParams(st, p);
		const int kind = static_cast<int>(rnd() % 5);
		double ph = 0;
		const double f = 0.001 + (rnd() % 1000) / 4000.0;
		for(int n = 0; n < blocks; ++n)
		{
			if(walk)
			{
				for(auto& v : p)
				{
					const int step = static_cast<int>(rnd() % 9) - 4;
					v = static_cast<uint32_t>(std::max(0, std::min(127, static_cast<int>(v >> 7) + step))) << 7;
					if(rnd() % 50 == 0) v = (rnd() % 128) << 7;
				}
				ref.setFx(track, p);
				engine::TrackFx::setParams(st, p);
			}
			mixref::MixerRef::Block in, out;
			for(auto& s : in)
			{
				ph += f;
				switch(kind)
				{
				case 0: s = static_cast<int32_t>(std::fmod(ph, 1.0) * 16777215.0) - 8388608; break;
				case 1: s = engine::d56::sx24(rnd()); break;
				case 2: s = static_cast<int32_t>(std::sin(ph * 6.283) * 8388607.0); break;
				case 3: s = 0; break;
				default: s = (static_cast<int>(ph * 8) & 1) ? 0x7fffff : -0x800000; break;
				}
			}
			if(!ref.runTrack(track, in, out, stopAt)) { std::printf("ref fault: %s\n", ref.fault().c_str()); return 1; }
			int32_t mine[32];
			fx.process(st, in.data(), mine);
			int diff = 0;
			if(all)
			{
				for(int i = 0; i < 32; ++i) diff += engine::d56::u24(mine[i]) != engine::d56::u24(out[i]);
			}
			else
			{
				for(uint32_t a = xFrom; a <= xTo; ++a) diff += engine::d56::u24(fx.scratch()[a]) != ref.peekX(a);
			}
			int sdiff = 0;
			for(int k = 0; k < 64; ++k)
				if(engine::d56::u24(st.y[k]) != ref.peekY(0x200 + 0x40 * static_cast<uint32_t>(track) + static_cast<uint32_t>(k)))
				{
					if(sdiff++ == 0 && bad < 5)
						std::printf("  state y[%02x]: mine %06x dsp %06x\n", k, engine::d56::u24(st.y[k]), ref.peekY(0x200 + 0x40 * static_cast<uint32_t>(track) + static_cast<uint32_t>(k)));
				}
			total += 32;
			if(diff || sdiff)
			{
				if(bad < 5)
				{
					std::printf("trial %d track %d kind %d block %d: %d samples, %d state words differ; params", t, track, kind, n, diff, sdiff);
					for(auto v : p) std::printf(" %u", v >> 7);
					std::printf("\n");
				}
				bad += diff + (sdiff ? 1 : 0);
				// resync from the DSP so one error doesn't cascade
				for(int k = 0; k < 64; ++k) st.y[k] = engine::d56::sx24(ref.peekY(0x200 + 0x40 * static_cast<uint32_t>(track) + static_cast<uint32_t>(k)));
			}
		}
	}
	std::printf("%s: %ld samples compared, %ld mismatches\n", argv[2], total, bad);
	return bad || sineBad ? 1 : 0;
}
