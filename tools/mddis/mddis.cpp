// mddis: disassemble one DSP program from a Machinedrum OS .syx.
// usage: mddis OS.syx <section 1|2> [from to]...   (hex P addresses; default: whole internal P)
// Output contains firmware-derived code: never commit it.
#include "../mdfw/Firmware.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/opcodes.h"
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace md::fw;

int main(int argc, char** argv)
{
	if (argc < 3) { std::fprintf(stderr, "usage: mddis OS.syx <1|2> [from to]...\n"); return 2; }
	const auto c = parseContainer(parseSysex(readFile(argv[1])));
	const int sec = std::atoi(argv[2]);
	const auto img = parseDspRecords(c.sections.at(sec).data);
	std::map<uint32_t, uint32_t> p;   // P space only (X/Y dumped separately with --data)
	for (const auto& r : img.records)
		if (r.space == Space::P)
			for (size_t k = 0; k < r.words.size(); ++k) p[r.addr + uint32_t(k)] = r.words[k];

	auto get = [&](uint32_t a) { auto it = p.find(a); return it == p.end() ? 0u : it->second; };
	dsp56k::Opcodes opcodes;
	dsp56k::Disassembler dis(opcodes);

	auto dump = [&](uint32_t from, uint32_t to) {
		for (uint32_t pc = from; pc < to;) {
			if (!p.count(pc)) { ++pc; continue; }
			std::string s;
			const auto len = dis.disassemble(s, get(pc), get(pc + 1), 0, 0, pc);
			std::printf("p:%06x  %06x%s  %s\n", pc, get(pc), len > 1 ? (" " + [&]{ char b[8]; std::snprintf(b, 8, "%06x", get(pc+1)); return std::string(b); }()).c_str() : "       ", s.c_str());
			pc += len ? len : 1;
		}
	};
	if (argc == 3) dump(0, 0x1000);
	for (int i = 3; i + 1 < argc; i += 2) dump(std::strtoul(argv[i], nullptr, 16), std::strtoul(argv[i + 1], nullptr, 16));
	return 0;
}
