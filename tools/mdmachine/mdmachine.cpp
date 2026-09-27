// mdmachine: list the OS machine table, or compute one machine's voice-slot words.
// usage: mdmachine OS.syx                         list machines
//        mdmachine OS.syx ID P1..P8               compute (P = 0-127 SYN values, scaled << 7)
//        mdmachine OS.syx ID -raw W1..W8          compute from raw 16-bit parameter words (hex)
#include "../../engine/MachineRunner.h"
#include "../mdfw/Firmware.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
	if(argc < 2) { std::fprintf(stderr, "usage: mdmachine OS.syx [ID P1..P8 | ID -raw W1..W8]\n"); return 2; }
	try
	{
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		md::engine::MachineRunner runner(std::move(c.sections.at(0).data));
		if(argc == 2)
		{
			for(const auto& m : runner.machines())
				std::printf("%3d %-6s fn=$%06x %s %s %s %s %s %s %s %s\n", m.id, m.name.c_str(), m.function,
					m.params[0].c_str(), m.params[1].c_str(), m.params[2].c_str(), m.params[3].c_str(),
					m.params[4].c_str(), m.params[5].c_str(), m.params[6].c_str(), m.params[7].c_str());
			return 0;
		}
		const auto id = static_cast<uint8_t>(std::atoi(argv[2]));
		const bool raw = argc > 3 && !std::strcmp(argv[3], "-raw");
		uint16_t params[8] = {};
		for(int k = 0; k < 8 && 3 + (raw ? 1 : 0) + k < argc; ++k)
		{
			const char* s = argv[3 + (raw ? 1 : 0) + k];
			params[k] = raw ? static_cast<uint16_t>(std::strtoul(s, nullptr, 16)) : static_cast<uint16_t>(std::atoi(s) << 7);
		}
		uint32_t out[32];
		const int n = runner.compute(id, params, out, 32);
		if(n < 0) { std::fprintf(stderr, "fault: %s\n", runner.faultReason().c_str()); return 1; }
		std::printf("count=%d instructions=%llu words:", n, static_cast<unsigned long long>(runner.lastInstructions()));
		for(int k = 1; k < n && k < 32; ++k) std::printf(" %06x", out[k] & 0xffffff);
		std::printf("\n");
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdmachine: %s\n", e.what()); return 1; }
	return 0;
}
