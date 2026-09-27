// mdhost: HostModel + VoiceEngine end to end. Assigns a machine to a track, sets its parameters, lets the
// smoothing settle, triggers it and writes that voice's output (int32, 24-bit signed) to a file.
// usage: mdhost OS.syx OUT.raw TRACK MACHINE_ID BLOCKS [P0 P1 ... P23]   (P = 0-127)
// env MDHOST_LFO="destTrack,destParam,shape1,shape2,type" sets the track's LFO;
// env MDHOST_SLOTS=path writes the voice slot (13 words) after every tick as text.
#include "../../engine/HostModel.h"
#include "../mdfw/Firmware.h"
#include "dsp56kEmu/dsp.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
	if(argc < 6) { std::fprintf(stderr, "usage: mdhost OS.syx OUT.raw TRACK MACHINE_ID BLOCKS [P0..P23]\n"); return 2; }
	try
	{
		const auto fw = md::fw::loadFirmware(argv[1]);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		md::engine::MachineRunner os(std::move(c.sections.at(0).data));
		md::engine::VoiceEngine voices(fw);
		md::engine::HostModel host(os, voices);

		const int track = std::atoi(argv[3]);
		const auto machine = static_cast<uint8_t>(std::atoi(argv[4]));
		const int blocks = std::atoi(argv[5]);
		host.setMachine(track, machine);
		for(int p = 0; p < 24 && 6 + p < argc; ++p)
			host.setParam(track, p, std::atoi(argv[6 + p]));

		if(const char* lfo = std::getenv("MDHOST_LFO"))
		{
			int v[5] = {};
			std::sscanf(lfo, "%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
			host.setLfo(track, v[0], v[1], v[2], v[3], v[4]);
		}
		std::FILE* slots = std::getenv("MDHOST_SLOTS") ? std::fopen(std::getenv("MDHOST_SLOTS"), "w") : nullptr;

		md::engine::VoiceEngine::Block b;
		host.trigger(track);
		std::FILE* f = std::fopen(argv[2], "wb");
		uint64_t dspInstr = 0;
		for(int n = 0; n < blocks; ++n)
		{
			if(!host.renderBlock(b)) { std::fprintf(stderr, "fault: %s\n", voices.faultReason().c_str()); return 1; }
			dspInstr += voices.instructionsLastBlock();
			if(slots)
			{
				for(int k = 0; k < 13; ++k)
					std::fprintf(slots, "%06x ", voices.dsp().memory().get(dsp56k::MemArea_Y, 0x800 + 0x40 * static_cast<uint32_t>(track) + static_cast<uint32_t>(k)));
				std::fprintf(slots, "\n");
			}
			std::fwrite(b[track].data(), 4, b[track].size(), f);
		}
		std::fclose(f);
		const auto* m = os.machine(machine);
		std::printf("track %d %s: %d blocks, DSP2 %.1f M instr/s\n", track + 1, m ? m->name.c_str() : "?", blocks,
			double(dspInstr) / blocks * 44100.0 / 32.0 / 1e6);
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdhost: %s\n", e.what()); return 1; }
	return 0;
}
