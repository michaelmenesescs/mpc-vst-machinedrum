#include "ParallelVoiceEngine.h"

#include <algorithm>

namespace md::engine
{
	ParallelVoiceEngine::ParallelVoiceEngine(const fw::Firmware& _fw, const int _groups)
	{
		const int n = std::clamp(_groups, 1, kVoices);
		m_groups.reserve(static_cast<size_t>(n));
		for(int g = 0; g < n; ++g)
			m_groups.push_back(std::make_unique<VoiceEngine>(_fw));
		m_groupOut.resize(static_cast<size_t>(n));
	}

	void ParallelVoiceEngine::setSlot(const int _voice, const uint32_t* _words, const int _count)
	{
		m_groups[static_cast<size_t>(groupOf(_voice))]->setSlot(_voice, _words, _count);
	}

	bool ParallelVoiceEngine::renderBlock(Block& _out)
	{
		const auto n = m_groups.size();
		std::vector<std::thread> threads;
		std::vector<uint8_t> ok(n, 0);
		threads.reserve(n - 1);
		for(size_t g = 1; g < n; ++g)
			threads.emplace_back([this, g, &ok] { ok[g] = m_groups[g]->renderBlock(m_groupOut[g]) ? 1 : 0; });
		ok[0] = m_groups[0]->renderBlock(m_groupOut[0]) ? 1 : 0;
		for(auto& t : threads)
			t.join();

		m_lastInstructions = 0;
		m_fault.clear();
		bool allOk = true;
		for(size_t g = 0; g < n; ++g)
		{
			m_lastInstructions += m_groups[g]->instructionsLastBlock();
			if(!ok[g])
			{
				allOk = false;
				if(m_fault.empty())
					m_fault = m_groups[g]->faultReason();
			}
		}
		if(!allOk)
			return false;

		for(int v = 0; v < kVoices; ++v)
			_out[v] = m_groupOut[static_cast<size_t>(groupOf(v))][v];
		return true;
	}
}
