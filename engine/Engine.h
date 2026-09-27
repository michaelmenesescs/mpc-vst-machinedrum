// Machinedrum One's engine: the OS's own control code (HostModel + MachineRunner), the voice DSP (VoiceEngine,
// emulated) and the mixer DSP's per-track effects and mix as native C++ (TrackFx, Mixer). One 32-sample block
// at a time: all 16 tracks, the dry main mix, the reverb/delay sends and each track's own output.
#pragma once
#include <array>
#include <memory>
#include <vector>

#include "HostModel.h"
#include "MachineRunner.h"
#include "Mixer.h"
#include "TrackFx.h"
#include "VoiceEngine.h"

namespace md::engine
{
	class Engine
	{
	public:
		static constexpr int kTracks = 16;
		static constexpr int kBlock = 32;

		// _osImage: the OS file's section 0 (the ColdFire OS), decompressed
		Engine(const fw::Firmware& _fw, std::vector<uint8_t> _osImage);
		~Engine();

		HostModel& host() { return *m_host; }
		const MachineRunner& os() const { return *m_os; }

		struct Output
		{
			Mixer::Output mix;													// main, sends, individual outputs
			std::array<std::array<int32_t, kBlock>, kTracks> tracks{};			// each track after its effects
		};
		bool render(Output& _out);
		const std::string& fault() const { return m_voices->faultReason(); }

	private:
		std::unique_ptr<MachineRunner> m_os;
		std::unique_ptr<VoiceEngine> m_voices;
		std::unique_ptr<HostModel> m_host;
		TrackFx::Tables m_tables;
		TrackFx m_fx;
		Mixer m_mixer;
		std::array<TrackFx::State, kTracks> m_state{};
		VoiceEngine::Block m_voiceOut{};
	};
}
