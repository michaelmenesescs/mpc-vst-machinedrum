// Splits the 16 voice slots across several VoiceEngine instances rendered in parallel threads, one physical
// DSP2 emulation per group. Each instance only ever receives setSlot() calls for the voices assigned to it;
// the rest stay at machine 0 (never triggered) forever, which VoiceEngine's own harness already renders as a
// fast, near-free clear (see VoiceEngine::installHarness). So each instance's cost is roughly the shared
// per-block baseline plus just its own subset's active-voice cost, and those run concurrently on separate
// cores instead of all 16 voices' cost serialized on one.
//
// This does not change what gets emulated (DSP2 is still one program run per group, exactly as VoiceEngine
// runs it alone), only how the 16 slots' work is scheduled across cores. See HANDOFF.md, "per-machine cost
// profiled" for the measurement this answers.
#pragma once
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "VoiceEngine.h"

namespace md::engine
{
	class ParallelVoiceEngine
	{
	public:
		static constexpr int kVoices = VoiceEngine::kVoices;
		static constexpr int kBlockFrames = VoiceEngine::kBlockFrames;
		static constexpr int kSlotWords = VoiceEngine::kSlotWords;
		using Block = VoiceEngine::Block;

		// _groups: how many VoiceEngine instances to split the 16 voices across (each on its own thread during
		// renderBlock). 1 disables parallelism (behaves like VoiceEngine, minus the thread-per-block overhead).
		ParallelVoiceEngine(const fw::Firmware& _fw, int _groups);

		void setSlot(int _voice, const uint32_t* _words, int _count = kSlotWords);
		bool renderBlock(Block& _out);

		uint64_t instructionsLastBlock() const { return m_lastInstructions; }
		const std::string& faultReason() const { return m_fault; }

	private:
		int groupOf(int _voice) const { return _voice % static_cast<int>(m_groups.size()); }

		std::vector<std::unique_ptr<VoiceEngine>> m_groups;
		std::vector<Block> m_groupOut;
		uint64_t m_lastInstructions = 0;
		std::string m_fault;
	};
}
