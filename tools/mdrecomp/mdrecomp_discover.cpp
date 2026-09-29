// mdrecomp-discover: static recompilation, step 1, for the Machinedrum voice DSP (DSP2).
// Traces every DSP instruction the interpreter executes while running every machine in the OS's
// descriptor table through a representative sweep of triggers and coefficients, and writes what
// the generator (recomp_gen2.py, from libs/dsp56300/tools/arm32jit_prototype/recomp/) needs to build
// basic blocks. Needs a dsp56300 build with -DDSP56K_RECOMP_DISCOVERY.
//
//   mdrecomp-discover <os.syx> <out.txt>
//
// Output format: identical to mnm_recomp_discover.cpp (see that file's header comment); consumed by
// the same recomp_gen2.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <vector>
#include <dlfcn.h>

#include "../../engine/VoiceEngine.h"
#include "../../engine/MachineRunner.h"
#include "../mdfw/Firmware.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/opcodes.h"
#include "dsp56kEmu/opcodeanalysis.h"

using namespace dsp56k;

namespace
{
	std::map<TWord, uint64_t> g_runCount;
	std::set<TWord> g_entries, g_loopEnds;
	TWord g_expectedNext = 0xffffffff;
	Opcodes g_ops;

	TWord lengthAt(DSP* d, TWord pc)
	{
		const TWord a = d->memory().get(MemArea_P, pc);
		Instruction ia = Nop, ib = Invalid;
		if(a) g_ops.getInstructionTypes(a, ia, ib);
		const auto len = Opcodes::getOpcodeLength(a, ia, ib);
		return len ? len : 1;
	}

	void hook(DSP* d, TWord pc)
	{
		++g_runCount[pc];
		if(pc != g_expectedNext) g_entries.insert(pc);
		if(d->regs().sr.var & SR_LF) g_loopEnds.insert(TWord(d->regs().la.var));
		g_expectedNext = pc + lengthAt(d, pc);
	}

	uintptr_t off(void* p) { Dl_info i{}; return p && dladdr(p, &i) ? uintptr_t(p) - uintptr_t(i.dli_fbase) : 0; }

	using Key = std::tuple<TWord, TWord, TWord>;
	struct Info { TWord len; int kind; bool parallel; size_t op, mv, alu; uint64_t count; bool moveAB, readsPC; int ccr; uint64_t mr, mw, ar, aw; };
	std::map<Key, Info> g_infos;

	void collect(DSP& d)
	{
		for(const auto& [pc, n] : g_runCount)
		{
			const TWord a = d.memory().get(MemArea_P, pc), b = d.memory().get(MemArea_P, pc + 1);
			const Key k{pc, a, b};
			auto it = g_infos.find(k);
			if(it != g_infos.end()) { it->second.count += n; continue; }
			Instruction ia = Nop, ib = Invalid;
			if(a) g_ops.getInstructionTypes(a, ia, ib);
			const TWord len = Opcodes::getOpcodeLength(a, ia, ib);
			const auto ri = d.getRecompInfo(pc);
			const auto flags = Opcodes::getFlags(ia, ib);
			RegisterMask written = RegisterMask::None, read = RegisterMask::None;
			Opcodes::getRegisters(written, read, a, ia, ib);
			constexpr auto ctrl = RegisterMask::PC | RegisterMask::LA | RegisterMask::LC | RegisterMask::SSH | RegisterMask::SSL |
			                      RegisterMask::SP | RegisterMask::SC | RegisterMask::EP | RegisterMask::SZ | RegisterMask::EMR |
			                      RegisterMask::MR | RegisterMask::OMR;
			int kind = 0;
			if(!ri.resolved || (flags & (OpFlagLoop | OpFlagRepDynamic | OpFlagRepImmediate)) || ia == Wait || ia == Ifcc ||
			   ia == Ifcc_U || ib == Ifcc || ib == Ifcc_U)
				kind = 2;
			else if((flags & (OpFlagBranch | OpFlagPopPC)) || (written & ctrl) != RegisterMask::None)
				kind = 1;
			bool moveAB = true;
			uint64_t pmr = 0, pmw = 0, par_ = 0, paw = 0;
			if(ri.parallel)
			{
				RegisterMask mw = RegisterMask::None, mr = RegisterMask::None;
				RegisterMask aw = RegisterMask::None, ar = RegisterMask::None;
				Opcodes::getRegisters(mw, mr, a, ib, Invalid);
				Opcodes::getRegisters(aw, ar, a, ia, Invalid);
				auto touches = [](RegisterMask m, RegisterMask acc) { return (m & acc) != RegisterMask::None; };
				moveAB = (touches(aw, RegisterMask::A) && touches(mw | mr, RegisterMask::A)) ||
				         (touches(aw, RegisterMask::B) && touches(mw | mr, RegisterMask::B));
				pmr = uint64_t(mr); pmw = uint64_t(mw); par_ = uint64_t(ar); paw = uint64_t(aw);
			}
			const bool readsPC = (read & RegisterMask::PC) != RegisterMask::None;
			const int ccr = int((read & RegisterMask::CCR) != RegisterMask::None) |
			                int((written & RegisterMask::CCR) != RegisterMask::None || (flags & OpFlagCCR)) << 1 |
			                int((flags & OpFlagCondition) != 0) << 2;
			g_infos[k] = {len ? len : 1, kind, ri.parallel, off(ri.op), off(ri.opMove), off(ri.opAlu), n, moveAB, readsPC, ccr, pmr, pmw, par_, paw};
		}
		g_runCount.clear();
	}
}

int main(int argc, char** argv)
{
	if(argc < 3) { std::fprintf(stderr, "usage: mdrecomp-discover <os.syx> <out.txt> [ROM_SAMPLES.bin]\n"); return 2; }
	try
	{
		const auto fwv = md::fw::loadFirmware(argv[1]);
		auto c = md::fw::parseContainer(md::fw::parseSysex(md::fw::readFile(argv[1])));
		md::engine::MachineRunner os(std::move(c.sections.at(0).data));
		md::engine::VoiceEngine eng(fwv);
		// The ROM machines' sample memory (ROM_SAMPLES.bin, tools/mdkits): without it their playback code never runs, so the
		// recompiler would leave it on the slow interpreter (on the Force ROM tracks were the most expensive, 2026-09-29).
		if(argc > 3)
			if(std::FILE* rf = std::fopen(argv[3], "rb"))
			{
				char magic[4] = {};
				if(std::fread(magic, 1, 4, rf) == 4 && !std::memcmp(magic, "MDS1", 4))
				{
					std::vector<uint32_t> words;
					uint32_t head[2];
					while(std::fread(head, 4, 2, rf) == 2 && head[1] > 0 && head[1] < 0x800000)
					{
						words.resize(head[1]);
						if(std::fread(words.data(), 4, head[1], rf) != head[1]) break;
						eng.writeP(head[0], words.data(), words.size());
					}
				}
				std::fclose(rf);
			}
		using md::engine::VoiceEngine;
		DSP::s_recompTraceHook = &hook;

		std::mt19937 rng(7);
		std::uniform_int_distribution<int> synDist(0, 127);

		for(const auto& m : os.machines())
		{
			if(m.id == 0 || m.id == 1) continue;	// GND-- / never-triggered: no render code of interest

			g_expectedNext = 0xffffffff;
			// Sweep several coefficient sets per machine (varied SYN1-8) so parameter-dependent branches
			// (filter modes, one-shot vs. looped playback, sample-rate reducer states...) get exercised, not
			// just one fixed patch.
			for(int sweep = 0; sweep < 6; ++sweep)
			{
				uint16_t params[8];
				for(auto& p : params) p = static_cast<uint16_t>(synDist(rng) << 7);

				uint32_t words[32] = {};
				const int n = os.compute(m.id, params, words, 32, true);	// as the OS on a trigger tick (some machines only compute then)
				if(n < 0) continue;
				words[0] = static_cast<uint32_t>(m.id) + 1;	// trigger this machine on voice 0 (its code is id + 1)

				VoiceEngine::Block b;
				eng.setSlot(0, words, VoiceEngine::kSlotWords);
				for(int blk = 0; blk < 64; ++blk)
				{
					if(!eng.renderBlock(b)) break;
					if(blk == 40)	// re-trigger mid-decay to exercise restart/retrigger paths too
					{
						uint32_t retrig[32];
						std::copy(std::begin(words), std::end(words), retrig);
						eng.setSlot(0, retrig, VoiceEngine::kSlotWords);
					}
				}
			}
			collect(eng.dsp());
			std::fprintf(stderr, "%-8s (id %3d) traced, %zu distinct instructions so far\n", m.name.c_str(), int(m.id), g_infos.size());
		}
	}
	catch(const std::exception& e) { std::fprintf(stderr, "mdrecomp-discover: %s\n", e.what()); return 1; }

	FILE* f = std::fopen(argv[2], "w");
	if(!f) { std::perror(argv[2]); return 1; }
	for(const auto& [k, i] : g_infos)
		std::fprintf(f, "I %06x %06x %06x %u %d %d %zx %zx %zx %llu %d %d %d %llx %llx %llx %llx\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), i.len,
		             i.kind, int(i.parallel), i.op, i.mv, i.alu, (unsigned long long)i.count, int(i.moveAB), int(i.readsPC), i.ccr,
		             (unsigned long long)i.mr, (unsigned long long)i.mw, (unsigned long long)i.ar, (unsigned long long)i.aw);
	for(auto e : g_entries) std::fprintf(f, "E %06x\n", e);
	for(auto l : g_loopEnds) std::fprintf(f, "L %06x\n", l);
	std::fclose(f);
	return 0;
}
