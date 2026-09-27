// mdProbe: local investigation tool (mpc-vst-machinedrum), not part of gearmulator-md-mm.
// Boots md::Hardware from a user-supplied MD OS 1.63 ROM (first-run flash initialisation is done once
// and cached to a scratch file), then runs a script of actions with the mddsp.cpp HI08 trace switched on
// only where asked. Prints "-- action --" markers and the audio peak of each step to stderr.
//
// usage: mdProbe ROM.bin FLASHCACHE [action]...
//   trace:on | trace:off
//   wait:FRAMES
//   trig:N[:HOLD]            press front-panel trigger N (1..16), release after HOLD frames (default 2048)
//   note:CH:NOTE:VEL         MIDI note-on (CH 0-based)
//   off:CH:NOTE              MIDI note-off
//   cc:CH:CC:VAL             MIDI control change
//   sysex:HEXBYTES           e.g. sysex:f000203c02005b000001f7
//   panel:NAME               tap a panel control (Kit, Enter, Exit, Up, Down, Left, Right, Play, Stop, ...)
//   wav:PATH                 start recording the stereo output as float32 raw to PATH
#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdfrontpanel.h"
#include "baseLib/filesystem.h"
#include "synthLib/midiTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace md { extern bool g_mdTraceOn; extern bool g_mdEssiDumpOn; extern bool g_mdCfTraceOn; extern uint32_t g_mdWatchBegin, g_mdWatchEnd; extern uint32_t g_mdBp[8], g_mdBpCount, g_mdBpDumpReg, g_mdBpDumpLen; extern bool g_mdCountOn; extern uint64_t g_mdCount[4]; }

namespace
{
	std::FILE* g_wav = nullptr;

	float run(md::Hardware& _hw, uint32_t _frames)
	{
		constexpr uint32_t block = 128;
		std::vector<float> l(block), r(block);
		float peak = 0;
		for(uint32_t done = 0; done < _frames; done += block)
		{
			const auto n = std::min(block, _frames - done);
			synthLib::TAudioOutputs out{};
			out[0] = l.data(); out[1] = r.data();
			_hw.processAudio(out, n, 0);
			for(uint32_t i = 0; i < n; ++i)
			{
				peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
				if(g_wav) { std::fwrite(&l[i], 4, 1, g_wav); std::fwrite(&r[i], 4, 1, g_wav); }
			}
		}
		return peak;
	}

	std::vector<int> ints(const std::string& _s)
	{
		std::vector<int> v;
		for(size_t p = _s.find(':'); p != std::string::npos; p = _s.find(':', p + 1))
			v.push_back(std::atoi(_s.c_str() + p + 1));
		return v;
	}

	const std::map<std::string, md::PanelControl> g_panel = {
		{"Kit", md::PanelControl::Kit}, {"Enter", md::PanelControl::Enter}, {"Exit", md::PanelControl::Exit},
		{"Up", md::PanelControl::Up}, {"Down", md::PanelControl::Down}, {"Left", md::PanelControl::Left},
		{"Right", md::PanelControl::Right}, {"Play", md::PanelControl::Play}, {"Stop", md::PanelControl::Stop},
		{"Function", md::PanelControl::Function}, {"Record", md::PanelControl::Record},
		{"Track1", md::PanelControl::Track1}, {"Track2", md::PanelControl::Track2},
	};

	bool press(md::Hardware& _hw, md::PanelControl _c, uint32_t _hold)
	{
		const auto p = md::panelPacket(md::MachineModel::Machinedrum, _c);
		if(!p) return false;
		_hw.sendPanelEvent(p->row, p->mask);
		std::cerr << "   peak(press)=" << run(_hw, _hold) << '\n';
		_hw.sendPanelEvent(p->row, 0);
		return true;
	}
}

int main(int argc, char** argv)
{
	if(argc < 3) { std::cerr << "usage: mdProbe ROM.bin FLASHCACHE [action]...\n"; return 2; }
	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, argv[1])) { std::cerr << "cannot read ROM\n"; return 1; }

	std::vector<uint8_t> flash;
	baseLib::filesystem::readFile(flash, argv[2]);
	if(flash.empty())
	{
		std::cerr << "first-run flash initialisation (cached to " << argv[2] << ")\n";
		md::Hardware init(rom, argv[1], md::MachineModel::Machinedrum);
		if(!init.isValid()) { std::cerr << "ROM not valid\n"; return 1; }
		init.disqualifyFactoryFlashCache();
		for(uint32_t f = 0; f < md::g_samplerate * 18 && !init.isFactoryFlashReadyForReboot(); f += 128)
			init.advance(128);
		if(!init.isFactoryFlashReadyForReboot()) { std::cerr << "flash init did not complete\n"; return 1; }
		flash = init.copyFlashData();
		std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(flash.data()), static_cast<std::streamsize>(flash.size()));
	}

	md::Hardware hw(rom, argv[1], md::MachineModel::Machinedrum, {}, {}, flash);
	if(!hw.isValid()) { std::cerr << "hardware not valid\n"; return 1; }
	uint32_t boot = 0;
	const uint32_t bootFrames = std::getenv("MDPROBE_BOOT") ? static_cast<uint32_t>(std::atoi(std::getenv("MDPROBE_BOOT"))) : md::g_samplerate * 20;
	for(; boot < bootFrames; boot += 128) hw.advance(128);
	std::cerr << "-- booted after " << boot << " frames, midiReady=" << hw.isFirmwareMidiReady() << " --\n";

	for(int a = 3; a < argc; ++a)
	{
		const std::string s = argv[a];
		const std::string kind = s.substr(0, s.find(':'));
		const auto v = ints(s);
		std::cerr << "-- #" << a << " " << s << " --\n";
		if(s == "trace:on") { md::g_mdTraceOn = true; continue; }
		if(s == "trace:off") { md::g_mdTraceOn = false; continue; }
		if(kind == "watch") { md::g_mdWatchBegin = static_cast<uint32_t>(std::strtoul(s.c_str() + 6, nullptr, 16)); md::g_mdWatchEnd = static_cast<uint32_t>(std::strtoul(s.c_str() + s.rfind(':') + 1, nullptr, 16)); continue; }
		if(kind == "dis68k")
		{
			const auto from = static_cast<uint32_t>(std::strtoul(s.c_str() + 7, nullptr, 16));
			const auto to = static_cast<uint32_t>(std::strtoul(s.c_str() + s.rfind(':') + 1, nullptr, 16));
			for(uint32_t pc = from; pc < to;)
			{
				char buf[256];
				const auto len = hw.traceUc().disassemble(pc, buf);
				std::fprintf(stderr, "   %07x  %s\n", pc, buf);
				pc += len ? len : 2;
			}
			continue;
		}
		if(kind == "peek32")
		{
			const auto from = static_cast<uint32_t>(std::strtoul(s.c_str() + 7, nullptr, 16));
			const auto to = static_cast<uint32_t>(std::strtoul(s.c_str() + s.rfind(':') + 1, nullptr, 16));
			for(uint32_t a = from; a < to; a += 16)
			{
				std::fprintf(stderr, "   %08x ", a);
				for(uint32_t k = 0; k < 16 && a + k < to; k += 4)
					std::fprintf(stderr, " %04x%04x", hw.traceUc().read16(a + k), hw.traceUc().read16(a + k + 2));
				std::fprintf(stderr, "\n");
			}
			continue;
		}
		if(kind == "bp")	// bp:PC[,PC...][:REG:LEN]  REG 0-7 d0-d7, 8-15 a0-a7; bp:0 clears
		{
			md::g_mdBpCount = 0;
			const auto list = s.substr(3, s.find(':', 3) == std::string::npos ? std::string::npos : s.find(':', 3) - 3);
			for(size_t p = 0; p < list.size() && md::g_mdBpCount < 8;)
			{
				const auto e = list.find(',', p);
				const auto pc = static_cast<uint32_t>(std::strtoul(list.substr(p, e - p).c_str(), nullptr, 16));
				if(pc) md::g_mdBp[md::g_mdBpCount++] = pc;
				if(e == std::string::npos) break;
				p = e + 1;
			}
			if(v.size() >= 3) { md::g_mdBpDumpReg = static_cast<uint32_t>(v[v.size() - 2]); md::g_mdBpDumpLen = static_cast<uint32_t>(v.back()); }
			continue;
		}
		if(kind == "count")	// count:FRAMES - 68k instructions by region over FRAMES frames
		{
			for(auto& n : md::g_mdCount) n = 0;
			md::g_mdCountOn = true;
			run(hw, static_cast<uint32_t>(v.at(0)));
			md::g_mdCountOn = false;
			const double sec = double(v.at(0)) / md::g_samplerate;
			std::fprintf(stderr, "   68k instr/s: param+LFO(isram) %.0f  LFO shapes %.0f  machine fns %.0f  total CPU %.0f\n",
				md::g_mdCount[0] / sec, md::g_mdCount[1] / sec, md::g_mdCount[2] / sec, md::g_mdCount[3] / sec);
			continue;
		}
		if(kind == "dspregs")	// dspregs:N - DSP N peripheral/state words
		{
			auto& dsp = hw.traceDsp(static_cast<uint32_t>(v.at(0)));
			auto* periph = dsp.getPeriph(0);
			std::fprintf(stderr, "   pc=%06x sr=%06x omr=%06x", dsp.getPC().toWord(), dsp.getSR().toWord(), dsp.regs().omr.var);
			for(uint32_t a : {0xfffff9u, 0xfffff8u, 0xfffff7u, 0xfffff6u, 0xfffffeu, 0xffffffu, 0xfffffdu})
				std::fprintf(stderr, " X:%06x=%06x", a, periph->read(a, dsp56k::Nop));
			for(uint32_t a : {0x147fffu, 0x140u, 0x141u, 0x142u, 0x7ffu})
				std::fprintf(stderr, " Y:%06x=%06x", a, dsp.memory().get(dsp56k::MemArea_Y, a));
			std::fprintf(stderr, "\n");
			continue;
		}
		if(kind == "dumpmem")	// dumpmem:FROM:TO:PATH - raw ColdFire memory to a file
		{
			const auto p1 = s.find(':'), p2 = s.find(':', p1 + 1), p3 = s.find(':', p2 + 1);
			const auto from = static_cast<uint32_t>(std::strtoul(s.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 16));
			const auto to = static_cast<uint32_t>(std::strtoul(s.substr(p2 + 1, p3 - p2 - 1).c_str(), nullptr, 16));
			std::FILE* f = std::fopen(s.substr(p3 + 1).c_str(), "wb");
			for(uint32_t a = from; a < to; a += 2) { const uint16_t w = hw.traceUc().read16(a); const uint8_t b[2] = {static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w)}; std::fwrite(b, 1, 2, f); }
			std::fclose(f);
			continue;
		}
		if(s == "cf:on") { md::g_mdCfTraceOn = true; continue; }
		if(s == "cf:off") { md::g_mdCfTraceOn = false; continue; }
		if(s == "essi:on") { md::g_mdEssiDumpOn = true; continue; }
		if(s == "essi:off") { md::g_mdEssiDumpOn = false; continue; }
		if(s == "lcd")
		{
			const auto fp = hw.getFrontPanelSnapshot();
			for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; y += 2)
			{
				std::string line;
				for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				{
					const bool a = fp.getLcdPixel(x, y), b = fp.getLcdPixel(x, y + 1);
					line += a && b ? '#' : a ? '"' : b ? '.' : ' ';
				}
				std::cerr << "   |" << line << "|\n";
			}
			continue;
		}
		if(kind == "wav") { g_wav = std::fopen(s.substr(4).c_str(), "wb"); continue; }
		if(kind == "prof")
		{
			// PC histogram: sample both DSPs' PC after every 16-frame advance, for FRAMES frames.
			std::map<uint32_t, uint32_t> h[2]; uint64_t i0[2], c0[2];
			for(int d = 0; d < 2; ++d) { i0[d] = hw.traceDsp(d).getInstructionCounter(); c0[d] = hw.traceDsp(d).getCycles(); }
			uint32_t samples = 0;
			for(int f = 0; f < v.at(0); f += 16) { run(hw, 16); for(int d = 0; d < 2; ++d) ++h[d][hw.traceDsp(d).getPC().toWord()]; ++samples; }
			for(int d = 0; d < 2; ++d)
			{
				const auto di = hw.traceDsp(d).getInstructionCounter() - i0[d];
				const auto dc = hw.traceDsp(d).getCycles() - c0[d];
				std::cerr << "   dsp" << d << " instr=" << di << " cycles=" << dc << " instr/s=" << (double(di) * md::g_samplerate / v.at(0)) << " top PCs:";
				std::vector<std::pair<uint32_t,uint32_t>> top(h[d].begin(), h[d].end());
				std::sort(top.begin(), top.end(), [](auto& a, auto& b){ return a.second > b.second; });
				for(size_t k = 0; k < std::min<size_t>(8, top.size()); ++k)
				{ char b[40]; std::snprintf(b, sizeof(b), " %06x:%.1f%%", top[k].first, 100.0 * top[k].second / samples); std::cerr << b; }
				std::cerr << '\n';
			}
			continue;
		}
		if(kind == "adv") { for(int f = 0; f < v.at(0); f += 128) hw.advance(128); continue; }
		if(kind == "wait") { std::cerr << "   peak=" << run(hw, static_cast<uint32_t>(v.at(0))) << '\n'; continue; }
		if(kind == "trig")
		{
			press(hw, static_cast<md::PanelControl>(static_cast<int>(md::PanelControl::Trigger1) + v.at(0) - 1), v.size() > 1 ? v[1] : 2048);
			continue;
		}
		if(kind == "panel")
		{
			const auto it = g_panel.find(s.substr(6));
			if(it == g_panel.end()) { std::cerr << "unknown panel control\n"; return 1; }
			press(hw, it->second, 2048);
			std::cerr << "   peak=" << run(hw, 4096) << '\n';
			continue;
		}
		synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
		if(kind == "note") { ev.a = static_cast<uint8_t>(synthLib::M_NOTEON | v.at(0)); ev.b = static_cast<uint8_t>(v.at(1)); ev.c = static_cast<uint8_t>(v.at(2)); }
		else if(kind == "off") { ev.a = static_cast<uint8_t>(synthLib::M_NOTEOFF | v.at(0)); ev.b = static_cast<uint8_t>(v.at(1)); ev.c = 0; }
		else if(kind == "cc") { ev.a = static_cast<uint8_t>(synthLib::M_CONTROLCHANGE | v.at(0)); ev.b = static_cast<uint8_t>(v.at(1)); ev.c = static_cast<uint8_t>(v.at(2)); }
		else if(kind == "sysex")
		{
			const auto hex = s.substr(6);
			for(size_t i = 0; i + 1 < hex.size(); i += 2) ev.sysex.push_back(static_cast<uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
		}
		else { std::cerr << "unknown action " << s << '\n'; return 1; }
		hw.sendMidi(ev);
	}
	if(g_wav) std::fclose(g_wav);
	std::cerr << "mdProbe: done\n";
	return 0;
}
