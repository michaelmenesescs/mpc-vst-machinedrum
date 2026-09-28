// mdmix: the mixer DSP's per-track effect chain in the emulator (MixerRef) on a test signal.
// usage: mdmix OS.syx OUT.raw BLOCKS AMD AMF EQF EQG FLTF FLTW FLTQ SRR DIST   (0-127)
// Input: a 24-bit sawtooth + noise. Writes the output as int32 (24-bit signed).
#include "MixerRef.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
	if(argc < 13) { std::fprintf(stderr, "usage: mdmix OS.syx OUT.raw BLOCKS AMD AMF EQF EQG FLTF FLTW FLTQ SRR DIST\n"); return 2; }
	try
	{
		const auto fw = md::fw::loadFirmware(argv[1]);
		md::mixref::MixerRef ref(fw);
		uint32_t fx[9];
		for(int k = 0; k < 9; ++k) fx[k] = static_cast<uint32_t>(std::atoi(argv[4 + k]) << 7);
		ref.setFx(0, fx);
		std::FILE* f = std::fopen(argv[2], "wb");
		const int blocks = std::atoi(argv[3]);
		uint32_t seed = 1;
		int32_t phase = 0;
		uint64_t instr = 0;
		md::mixref::MixerRef::Block in, out;
		for(int n = 0; n < blocks; ++n)
		{
			for(auto& s : in)
			{
				seed = seed * 1664525u + 1013904223u;
				phase += 0x20000;
				s = ((phase << 8) >> 9) + (static_cast<int32_t>(seed) >> 12);
				s = (s << 8) >> 8;
			}
			if(!ref.runTrack(0, in, out)) { std::fprintf(stderr, "fault: %s\n", ref.fault().c_str()); return 1; }
			instr += ref.instructionsLastRun();
			std::fwrite(out.data(), 4, out.size(), f);
		}
		std::fclose(f);
		std::printf("%d blocks, %.0f instructions/block for one track\n", blocks, double(instr) / blocks);
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdmix: %s\n", e.what()); return 1; }
	return 0;
}
