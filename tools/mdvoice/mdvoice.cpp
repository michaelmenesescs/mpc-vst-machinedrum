// mdvoice: drive md::engine::VoiceEngine from the command line.
// usage: mdvoice OS.syx OUT.raw BLOCKS VOICE W0 W1 ... W12   (slot words in hex; W0 = trigger/machine code)
// Writes the given voice's samples as int32 (24-bit signed) to OUT.raw and prints instructions per block.
#include "../../engine/VoiceEngine.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv)
{
	if(argc < 6) { std::fprintf(stderr, "usage: mdvoice OS.syx OUT.raw BLOCKS VOICE W0 [W1..W12]\n"); return 2; }
	try
	{
		const auto fw = md::fw::loadFirmware(argv[1]);
		md::engine::VoiceEngine eng(fw);
		const int blocks = std::atoi(argv[3]);
		const int voice = std::atoi(argv[4]);
		std::vector<uint32_t> slot;
		for(int i = 5; i < argc; ++i) slot.push_back(static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 16)));
		eng.setSlot(voice, slot.data(), static_cast<int>(slot.size()));
		std::FILE* f = std::fopen(argv[2], "wb");
		md::engine::VoiceEngine::Block b;
		uint64_t total = 0;
		for(int n = 0; n < blocks; ++n)
		{
			if(!eng.renderBlock(b)) { std::fprintf(stderr, "fault: %s\n", eng.faultReason().c_str()); return 1; }
			total += eng.instructionsLastBlock();
			std::fwrite(b[voice].data(), 4, b[voice].size(), f);
		}
		std::fclose(f);
		std::printf("%d blocks, %.0f instructions/block avg, %.1f M instructions/s at 44.1 kHz\n", blocks,
			double(total) / blocks, double(total) / blocks * 44100.0 / 32.0 / 1e6);
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdvoice: %s\n", e.what()); return 1; }
	return 0;
}
