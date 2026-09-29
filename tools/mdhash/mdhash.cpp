// mdhash: a hash of 12 s of a fixed 16-track pattern (random machines incl. ROM, routing, triggers) through the whole engine:
// equal hashes = identical audio. Compare builds (recompiled vs plain interpreter, ARM vs x86, skip paths on/off).
// usage: md-hash <OS.syx> [ROM_SAMPLES.bin]   (with the ROM samples, the pattern plays two ROM machines too)
#include <cstdio>
#include <cstdint>
#include <vector>
#include "Engine.h"
#include "Firmware.h"
int main(int argc, char** argv)
{
	const auto fwv = md::fw::loadFirmware(argv[1]);
	auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
	md::engine::Engine eng(fwv, std::move(c.sections.at(0).data));
	if(argc > 2)
		if(FILE* rf = fopen(argv[2], "rb"))
		{
			char magic[4]; std::vector<uint32_t> words; uint32_t head[2];
			if(fread(magic, 1, 4, rf) == 4)
				while(fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
				{
					words.resize(head[1]);
					if(fread(words.data(), 4, head[1], rf) != head[1]) break;
					eng.voices().writeP(head[0], words.data(), words.size());
				}
			fclose(rf);
		}
	auto& h = eng.host();
	md::engine::Engine::Output out;
	uint64_t hash = 1469598103934665603ull;
	auto mix = [&](int64_t v){ hash ^= uint64_t(v); hash *= 1099511628211ull; };
	unsigned rs = 99; auto rnd = [&]{ rs = rs * 1664525u + 1013904223u; return rs >> 8; };
	for(int t = 0; t < 16; ++t){ h.setMachine(t, uint8_t(16 + (t * 7) % 60)); h.setParam(t, 13, 127); h.setParam(t, 17, 100); h.setParam(t, 18, uint8_t(rnd() % 128)); h.setParam(t, 19, 40); h.setParam(t, 20, 40); h.setParam(t, 21, 40); }
	for(int t = 0; t < 6; ++t) h.setRouting(t, 1 + t % 5);
	h.setMachine(5, 130); h.setMachine(9, 140);
	for(int b = 0; b < 44100 * 12 / 32; ++b)
	{
		if(b % 300 == 0) for(int k = 0; k < 4; ++k) h.trigger(rnd() % 16, 100);
		eng.render(out);
		for(int f = 0; f < 32; ++f) for(int ch = 0; ch < 2; ++ch){ mix(out.mix.main[f][ch]); for(int q = 0; q < 6; ++q) mix(out.mix.frame[f][q]); mix(out.mix.rev[f][ch]); mix(out.mix.del[f][ch]); }
	}
	printf("hash %016llx\n", (unsigned long long)hash);
}
