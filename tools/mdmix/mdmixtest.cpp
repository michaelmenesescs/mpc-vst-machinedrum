// mdmixtest: engine/Mixer against the mixer DSP's own mix code (MixerRef::runMix), word for word.
// usage: mdmixtest OS.syx [TRIALS]
#include "MixerRef.h"
#include "../../engine/Mixer.h"
#include "../../engine/Dsp56.h"
#include <cstdio>
#include <cstdlib>

using namespace md;
using engine::d56::u24;

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: mdmixtest OS.syx [TRIALS]\n"); return 2; }
	const auto fwv = fw::loadFirmware(argv[1]);
	const int trials = argc > 2 ? std::atoi(argv[2]) : 200;
	mixref::MixerRef ref(fwv);
	engine::TrackFx::Tables tables(fwv);
	engine::Mixer mixer(tables);
	uint32_t seed = 777;
	auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	long bad = 0, total = 0, nz = 0, nzf = 0, sat = 0;
	for(int t = 0; t < trials; ++t)
	{
		mixref::MixerRef::Block blocks[16];
		uint32_t mixw[16][5];
		std::array<uint32_t, 5> mixa[16];
		const int32_t* ptr[16];
		const int mainShare = static_cast<int>(rnd() % 11);	// 0..10 of 10: how many tracks go to the main outs
		for(int k = 0; k < 16; ++k)
		{
			const int kind = static_cast<int>(rnd() % 4);
			for(auto& s : blocks[k])
				s = kind == 0 ? 0 : kind == 1 ? engine::d56::sx24(rnd()) : kind == 2 ? ((rnd() & 1) ? 0x7fffff : -0x800000) : engine::d56::sx24(rnd()) >> 6;
			const int lev = static_cast<int>(rnd() % 128), vel = static_cast<int>(rnd() % 128), vol = static_cast<int>(rnd() % 128);
			const int32_t l = lev << 7;
			const auto v = static_cast<uint32_t>(static_cast<int32_t>((((l * l) >> 8) * vel) >> 17) * static_cast<int32_t>((static_cast<uint32_t>(vol << 7) * static_cast<uint32_t>(vol << 7)) >> 17));
			const uint32_t pan = (rnd() % 128) << 16, rev = ((rnd() % 128) << 7) * ((rnd() % 128) << 7) >> 5, del = ((rnd() % 128) << 7) * ((rnd() % 128) << 7) >> 5;
			const uint32_t route = static_cast<int>(rnd() % 10) < mainShare ? 6 : rnd() % 6;
			const uint32_t w[5] = {route, v & 0xffffff, pan, rev & 0xffffff, del & 0xffffff};
			for(int i = 0; i < 5; ++i) { mixw[k][i] = w[i]; mixa[k][i] = w[i]; }
			ptr[k] = blocks[k].data();
		}
		if(!ref.runMix(blocks, mixw)) { std::printf("ref fault: %s\n", ref.fault().c_str()); return 1; }
		engine::Mixer::Output out;
		mixer.process(ptr, mixa, out);
		int diff = 0;
		for(int i = 0; i < 32; ++i)
			for(int c = 0; c < 2; ++c)
			{
				diff += u24(out.main[i][c]) != ref.peekX(0x180 + 2 * i + c);
				diff += u24(out.rev[i][c]) != ref.peekX(0x1c0 + 2 * i + c);
				diff += u24(out.del[i][c]) != ref.peekX(0x600 + 2 * i + c);
			}
		for(int i = 0; i < 32; ++i)
			for(int c = 0; c < 6; ++c)
				diff += u24(out.frame[i][c]) != ref.peekX(0x400 + 6 * i + c);
		total += 32 * 12;
		for(int i = 0; i < 64; ++i) { nz += ref.peekX(0x180 + i) != 0; nz += ref.peekX(0x1c0 + i) != 0; nz += ref.peekX(0x600 + i) != 0; }
		for(int i = 0; i < 192; ++i) { nzf += ref.peekX(0x400 + i) != 0; sat += ref.peekX(0x400 + i) == 0x7fffff || ref.peekX(0x400 + i) == 0x800000 || ref.peekX(0x180 + i % 64) == 0x7fffff; }
		if(diff && bad < 5)
		{
			std::printf("trial %d: %d words differ (main L0 mine %06x dsp %06x; frame0", t, diff, u24(out.main[0][0]), ref.peekX(0x180));
			for(int c = 0; c < 6; ++c) std::printf(" %06x/%06x", u24(out.frame[0][c]), ref.peekX(0x400 + c));
			std::printf(")\n");
		}
		bad += diff;
	}
	std::printf("mix: %ld words compared, %ld mismatches (non-zero: %ld main/send words, %ld frame words; %ld saturated)\n", total, bad, nz, nzf, sat);
	return bad ? 1 : 0;
}
