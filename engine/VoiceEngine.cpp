// Harness structure adapted from shnolk/monomodule src/core/dsp/DspEngine.cpp (AGPL-3.0).
#include "VoiceEngine.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/audio.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

namespace md::engine
{
	using namespace dsp56k;

	namespace
	{
		// Memory as gearmulator-md-mm models the Machinedrum: external SRAM from $020000 is shared by P, X and Y.
		constexpr TWord kSizeP = 0x800000, kSizeXY = 0x800000, kBridge = 0x020000;

		// DSP2 state after the ColdFire's first-stage loader, read from a running gearmulator-md-mm (OS 1.63):
		// the full SPS-1 UW memory map (AAR0-3) and OMR.
		constexpr TWord kAar[4] = {0x100539, 0x140639, 0x180539, 0x1c0639};
		constexpr TWord kOmr = 0x00498d;

		// Program addresses (OS 1.63 voice program, see docs/PROTOCOL.md)
		constexpr TWord kEntry = 0x24;			// after the vector table: init, then bra $64
		constexpr TWord kHostGroupTx = 0x73;	// movep a1,x:HTX every 4 voices (ColdFire pacing): removed
		constexpr TWord kAfterRender = 0xb5;	// voice rendered into Y:(y:$140); original: frame sync + link DMA
		constexpr TWord kNextVoice = 0xd5;		// advance to the next voice
		constexpr TWord kEndOfBlock = 0xe2;		// all 16 voices done; original: RAM-R input position, bra $64
		constexpr TWord kBlockStart = 0x64;
		constexpr TWord kStub = 0x0c00;			// free internal P RAM (the program's internal P ends at $3e2)

		constexpr uint64_t kMaxInstrInit = 200'000'000;
		constexpr uint64_t kMaxInstrBlock = 20'000'000;
		constexpr size_t kWordsPerBlock = VoiceEngine::kVoices * VoiceEngine::kBlockFrames;
	}

	VoiceEngine::VoiceEngine(const fw::Firmware& _fw) : m_fw(_fw)
	{
		m_validator = std::make_unique<DefaultMemoryValidator>();
		m_mem = std::make_unique<Memory>(*m_validator, kSizeP, kSizeXY, kBridge);
		m_periphX = std::make_unique<Peripherals56303>();
		m_periphY = std::make_unique<PeripheralsNop>();
		m_dsp = std::make_unique<DSP>(*m_mem, m_periphX.get(), m_periphY.get());
		{
			// As gearmulator-md-mm configures its DSPs: the program keeps code in the vector area.
			auto cfg = m_dsp->getJit().getConfig();
			cfg.dynamicFastInterrupts = true;
			cfg.aguSupportBitreverse = true;
			cfg.linkJitBlocks = false;
			m_dsp->getJit().setConfig(cfg);
		}
		m_periphX->getHI08().setRXRateLimit(0);
		m_periphX->getHI08().setTransmitDataAlwaysEmpty(true);

		// The program's init starts both ESSI ports (ESSI0 = link to the mixer DSP, ESSI1 = codec ADC). The
		// harness replaces the link and does not use the ADC: feed silence and discard output, never block.
		for(auto* essi : {&m_periphX->getEssi0(), &m_periphX->getEssi1()})
		{
			essi->setReadRxCallback([](uint64_t& _frameIndex, Audio::RxFrame& _frame)
			{
				_frame.resize(2);
				_frame[0] = Audio::RxSlot{};
				_frame[1] = Audio::RxSlot{};
				++_frameIndex;
			});
			essi->setWriteTxCallback([](uint64_t& _frameIndex, const Audio::TxFrame&) { ++_frameIndex; });
		}
		reset();
	}

	VoiceEngine::~VoiceEngine() = default;

	void VoiceEngine::loadImage(const fw::DspImage& _img)
	{
		for(const auto& r : _img.records)
		{
			for(size_t k = 0; k < r.words.size(); ++k)
			{
				const TWord a = r.addr + static_cast<TWord>(k);
				// P space, and anything in the shared external range, must go through memWriteP so the JIT sees it.
				if(r.space == fw::Space::P || a >= kBridge)
					m_dsp->memWriteP(a, r.words[k]);
				else
					m_dsp->memWrite(r.space == fw::Space::X ? MemArea_X : MemArea_Y, a, r.words[k]);
			}
		}
	}

	void VoiceEngine::installHarness()
	{
		Assembler as;
		auto emit = [&](TWord& _pc, const std::string& _text)
		{
			const auto r = as.assemble(_text.c_str());
			if(!r.success())
				throw std::runtime_error("voice harness: cannot assemble '" + _text + "'");
			m_dsp->memWriteP(_pc++, r.word[0]);
			if(r.wordCount > 1)
				m_dsp->memWriteP(_pc++, r.word[1]);
		};
		auto hex = [](TWord _v) { std::ostringstream o; o << "$" << std::hex << _v; return o.str(); };

		// 0) the silent-voice render ($10008f: 32 zeros into Y:(r7)) pads its time with a 32 x 50 nop loop so
		//    the hardware's block timing is even; the output is the same without it
		m_dsp->memWriteP(0x100093, 0x000000);
		m_dsp->memWriteP(0x100094, 0x000000);

		// 1) no per-group handshake word to the host
		m_dsp->memWriteP(kHostGroupTx, 0x000000);

		// 2) after each voice's render: send its 32 samples (Y:(y:$140)) to the host, then continue
		{
			TWord pc = kAfterRender;
			m_dsp->memWriteP(pc++, 0x0af080);	// jmp >stub
			m_dsp->memWriteP(pc++, kStub);
		}
		{
			TWord pc = kStub;
			emit(pc, "move y:>$140,r0");
			emit(pc, "move #>$ffffff,m0");
			emit(pc, "do #32," + hex(pc + 3));	// DO, not REP: REP over a peripheral access is unsafe in the JIT
			emit(pc, "movep y:(r0)+,x:<<$ffffc7");
			emit(pc, "nop");
			m_dsp->memWriteP(pc++, 0x0af080);	// jmp >next voice
			m_dsp->memWriteP(pc++, kNextVoice);
		}

		// 3) after all 16 voices: wait for the host's "go" word (the host updates the voice slots meanwhile)
		{
			TWord pc = kEndOfBlock;
			emit(pc, "jclr #0,x:<<$ffffc3," + hex(kEndOfBlock));
			emit(pc, "movep x:<<$ffffc6,a");
			emit(pc, "jmp " + hex(kBlockStart));
		}
	}

	void VoiceEngine::reset()
	{
		m_fault.clear();
		m_dsp->resetHW();
		loadImage(m_fw.dspA);	// section 1 = voice program (docs/PROTOCOL.md)
		installHarness();
		for(int i = 0; i < 4; ++i)
			m_periphX->write(0xfffff9 - static_cast<TWord>(i), kAar[i]);
		m_dsp->regs().omr.var = kOmr;
		auto& hi = m_periphX->getHI08();
		hi.clearRX();
		while(hi.hasTX())
			hi.readTX();
		m_dsp->setPC(kEntry);

		// Init runs into the block loop; the first (silent) block comes out and the DSP then waits for "go".
		if(!runUntilTx(kWordsPerBlock, kMaxInstrInit))
			throw std::runtime_error("voice DSP init did not complete: " + m_fault);
		while(hi.hasTX())
			hi.readTX();
	}

	void VoiceEngine::setSlot(const int _voice, const uint32_t* _words, const int _count)
	{
		const TWord base = kSlotBase + kSlotStride * static_cast<TWord>(_voice);
		for(int k = 0; k < _count; ++k)
			m_dsp->memWrite(MemArea_Y, base + static_cast<TWord>(k), _words[k] & 0xffffff);
	}

	bool VoiceEngine::runUntilTx(const size_t _words, const uint64_t _maxInstructions)
	{
		auto& hi = m_periphX->getHI08();
		const auto start = m_dsp->getInstructionCounter();
		static const bool debug = std::getenv("MDV_DEBUG") != nullptr;
		uint64_t nextReport = start + 5'000'000;
		while(hi.txData().size() < _words)
		{
			m_dsp->exec();
			if(debug && m_dsp->getInstructionCounter() > nextReport)
			{
				std::fprintf(stderr, "  PC=$%06x instr=%llu tx=%zu\n", m_dsp->getPC().toWord(),
					static_cast<unsigned long long>(m_dsp->getInstructionCounter() - start), hi.txData().size());
				nextReport += 5'000'000;
			}
			if(m_dsp->getInstructionCounter() - start > _maxInstructions)
			{
				std::ostringstream o;
				o << "instruction budget exceeded at PC=$" << std::hex << m_dsp->getPC().toWord() << std::dec
				  << " with " << hi.txData().size() << "/" << _words << " words";
				m_fault = o.str();
				return false;
			}
		}
		m_lastInstructions = m_dsp->getInstructionCounter() - start;
		return true;
	}

	bool VoiceEngine::renderBlock(Block& _out)
	{
		auto& hi = m_periphX->getHI08();
		const TWord go = 1;
		hi.writeRX(&go, 1);
		if(!runUntilTx(kWordsPerBlock, kMaxInstrBlock))
			return false;
		for(auto& voice : _out)
			for(auto& s : voice)
			{
				const TWord t = hi.readTX() & 0xffffff;
				s = static_cast<int32_t>(t << 8) >> 8;
			}
		return true;
	}
}
