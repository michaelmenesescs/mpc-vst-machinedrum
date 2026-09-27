#include "Engine.h"

namespace md::engine
{
	Engine::Engine(const fw::Firmware& _fw, std::vector<uint8_t> _osImage)
		: m_tables(_fw), m_fx(m_tables), m_mixer(m_tables)
	{
		m_os = std::make_unique<MachineRunner>(std::move(_osImage));
		m_voices = std::make_unique<VoiceEngine>(_fw);
		m_host = std::make_unique<HostModel>(*m_os, *m_voices);
		for(auto& s : m_state)
			TrackFx::init(s);
	}

	Engine::~Engine() = default;

	bool Engine::render(Output& _out)
	{
		if(!m_host->renderBlock(m_voiceOut))
			return false;
		std::array<const int32_t*, kTracks> in{};
		std::array<std::array<uint32_t, 5>, kTracks> mix{};
		for(int t = 0; t < kTracks; ++t)
		{
			const auto& mi = m_host->mixerInput(t);
			TrackFx::setParams(m_state[t], mi.fx.data());
			m_fx.process(m_state[t], m_voiceOut[t].data(), _out.tracks[t].data());
			in[t] = _out.tracks[t].data();
			mix[t] = mi.mix;
		}
		m_mixer.process(in.data(), mix.data(), _out.mix);
		return true;
	}
}
