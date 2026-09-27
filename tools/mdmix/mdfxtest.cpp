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

using namespace md;

int main(int argc, char** argv)
{
	if(argc < 3) { std::fprintf(stderr, "usage: mdfxtest OS.syx amd|all [TRIALS] [BLOCKS]\n"); return 2; }
	const auto fwv = fw::loadFirmware(argv[1]);
	const bool all = !std::strcmp(argv[2], "all");
	const bool eqStage = !std::strcmp(argv[2], "eq");
	const int trials = argc > 3 ? std::atoi(argv[3]) : 50;
	const int blocks = argc > 4 ? std::atoi(argv[4]) : 40;
	mixref::MixerRef ref(fwv);
	engine::TrackFx::Tables tables(fwv);

	// The tables: check the generated sine against the DSP's own
	int sineBad = 0;
	for(uint32_t a = 0x148000; a < 0x150000; ++a)
		if(engine::d56::u24(tables[a]) != ref.peekX(a)) ++sineBad;
	std::printf("sine table: %d/32768 words differ\n", sineBad);

	engine::TrackFx fx(tables);
	fx.stopAfter = all ? engine::TrackFx::Stop::None : eqStage ? engine::TrackFx::Stop::AfterEq : engine::TrackFx::Stop::AfterAmd;
	const uint32_t stopAt = all ? 0x25e : eqStage ? 0x134 : 0xd2;
	const uint32_t xFrom = eqStage ? 0x8 : 0xb, xTo = 0x2b;
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
