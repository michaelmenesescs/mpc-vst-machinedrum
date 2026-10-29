// Runs the Machinedrum voice DSP (DSP2) program from the user's OS file on its own, in the dsp56300
// emulator: no ColdFire, no mixer DSP. The hardware's link DMA and frame sync are replaced by a small
// harness stub (the Monomodule approach): each 32-sample block renders all 16 voices, and every voice's
// 32 samples go out over the host port instead of ESSI0. The host fills the 16 voice slots (Y:$800 +
// $40*k: word 0 = trigger/machine code, words 1-12 = machine coefficients) directly in DSP memory
// between blocks. See docs/PROTOCOL.md.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../tools/mdfw/Firmware.h"

namespace dsp56k { class DSP; class Memory; class Peripherals56303; class PeripheralsNop; class DefaultMemoryValidator; }

namespace md::engine
{
	class VoiceEngine
	{
	public:
		static constexpr int kVoices = 16;
		static constexpr int kBlockFrames = 32;
		static constexpr int kSlotWords = 32;	// machine functions return 0-21 words (the slot is 0x40 wide); 13 truncated EFM-CY and dropped the trigger of 0-word machines
		static constexpr uint32_t kSlotBase = 0x800, kSlotStride = 0x40;

		using Block = std::array<std::array<int32_t, kBlockFrames>, kVoices>;	// [voice][frame], 24-bit signed

		explicit VoiceEngine(const fw::Firmware& _fw);
		~VoiceEngine();

		// Load the program, run its own init, install the harness. Throws on failure.
		void reset();

		// Write a voice slot (up to kSlotWords words) for the next block. Word 0 is the trigger/machine
		// code: non-zero only on the block the voice is (re)triggered; the DSP clears it.
		void setSlot(int _voice, const uint32_t* _words, int _count = kSlotWords);

		// Render one 32-sample block of all 16 voices. Returns false on a fault.
		bool renderBlock(Block& _out);

		// Write words into P memory (e.g. the ROM machines' sample memory, which the MD copies from its sample flash at
		// boot - not part of the OS file). reset() reloads the program's own records, the ROM sample directory among them,
		// so write again after a reset.
		void writeP(uint32_t _addr, const uint32_t* _words, size_t _count);
		uint32_t readP(uint32_t _addr) const;

		// The DSP's memory (P, X, Y), registers and this class's per-block bookkeeping, between two blocks. restoreState puts
		// back exactly what saveState saw: the voice slots (in Y), every voice's state, the ROM samples. P goes back directly,
		// word by word where it changed, and a changed word in the loaded program's range is reported to the JIT; unchanged
		// code keeps every block the JIT compiled for it: what warmUp is for. Call between blocks, never while renderBlock runs.
		struct State;
		std::shared_ptr<State> saveState();
		void restoreState(const State& _state);

		// JIT warm-up: run _blocks[b][voice] (word count 0 = leave that slot as it is) for each b on this DSP, then put its
		// state back (saveState/restoreState). The DSP's JIT compiles the code it ran now rather than at a voice's first hit,
		// and the engine then sounds as if nothing had run. Returns false on a DSP fault (the state is put back either way).
		static bool hasJit();	// false: interpreter / statically recompiled blocks (32-bit ARM), where nothing compiles at a first hit
		struct WarmSlot { std::array<uint32_t, kSlotWords> words{}; int count = 0; };
		using WarmBlock = std::array<WarmSlot, kVoices>;
		bool warmUp(const std::vector<WarmBlock>& _blocks);

		uint64_t instructionsLastBlock() const { return m_lastInstructions; }
		uint32_t voiceInstructions(int _voice) const { return m_voiceInstr[static_cast<size_t>(_voice)]; }	// DSP instructions the voice used in the last block
		int activeVoicesLastBlock() const { return m_lastActive; }	// voices that rendered (flag 1) in the last block
		const std::string& faultReason() const { return m_fault; }
		dsp56k::DSP& dsp() { return *m_dsp; }

	private:
		void loadImage(const fw::DspImage& _img);
		void installHarness();
		bool runUntilTx(size_t _words, uint64_t _maxInstructions);
		bool readBlock(Block& _out, uint64_t _maxInstructions);

		const fw::Firmware& m_fw;
		std::unique_ptr<dsp56k::DefaultMemoryValidator> m_validator;
		std::unique_ptr<dsp56k::Memory> m_mem;
		std::unique_ptr<dsp56k::Peripherals56303> m_periphX;
		std::unique_ptr<dsp56k::PeripheralsNop> m_periphY;
		std::unique_ptr<dsp56k::DSP> m_dsp;
		uint64_t m_lastInstructions = 0;
		int m_lastActive = 0;
		std::array<uint32_t, kVoices> m_voiceInstr{};
		std::string m_fault;
		uint32_t m_maxProgramP = 0;	// highest P address reset() wrote through memWriteP (the JIT tracks up to there)
	};
}
