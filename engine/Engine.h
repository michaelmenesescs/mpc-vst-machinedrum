// Machinedrum One's engine: the OS's own control code (HostModel + MachineRunner), the voice DSP (VoiceEngine,
// emulated) and the mixer DSP's per-track effects and mix as native C++ (TrackFx, Mixer). One 32-sample block
// at a time: all 16 tracks, the dry main mix, the reverb/delay sends and each track's own output.
#pragma once
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "HostModel.h"
#include "MachineRunner.h"
#include "Mixer.h"
#include "ParallelVoiceEngine.h"
#include "TrackFx.h"
#include "VoiceEngine.h"

namespace md::engine
{
	// TVoices: VoiceEngine (default, one DSP2 instance) or ParallelVoiceEngine (splits the 16 voices across
	// threads - see its header and HANDOFF.md, "per-machine cost profiled"). Template so both share this same
	// code; defined inline here (not in Engine.cpp, which is now just an include of this header, kept so
	// existing build commands that name engine/Engine.cpp as a source file still work) so each consumer gets
	// its own instantiation without needing whole-class explicit instantiation, which would force every
	// TVoices to support every constructor overload below.
	template<class TVoices = VoiceEngine>
	class EngineT
	{
	public:
		static constexpr int kTracks = 16;
		static constexpr int kBlock = 32;

		// _osImage: the OS file's section 0 (the ColdFire OS), decompressed. Constructs TVoices with just the
		// firmware (VoiceEngine's own constructor).
		EngineT(const fw::Firmware& _fw, std::vector<uint8_t> _osImage)
			: m_tables(_fw), m_fx(m_tables), m_mixer(m_tables)
		{
			m_os = std::make_unique<MachineRunner>(std::move(_osImage));
			m_voices = std::make_unique<TVoices>(_fw);
			init();
		}

		// As above, but forwards _voicesArg to TVoices' constructor too (e.g. ParallelVoiceEngine's group
		// count). Only ever instantiated for a TVoices whose constructor actually takes (firmware, _voicesArg).
		template<class TVoicesArg>
		EngineT(const fw::Firmware& _fw, std::vector<uint8_t> _osImage, TVoicesArg _voicesArg)
			: m_tables(_fw), m_fx(m_tables), m_mixer(m_tables)
		{
			m_os = std::make_unique<MachineRunner>(std::move(_osImage));
			m_voices = std::make_unique<TVoices>(_fw, _voicesArg);
			init();
		}

		~EngineT() = default;

		HostModel<TVoices>& host() { return *m_host; }
		const MachineRunner& os() const { return *m_os; }
		TVoices& voices() { return *m_voices; }

		struct Output
		{
			Mixer::Output mix;													// main, sends, individual outputs
			std::array<std::array<int32_t, kBlock>, kTracks> tracks{};			// each track after its effects
		};

		bool render(Output& _out)
		{
			if(!m_host->renderBlock(m_voiceOut))
				return false;
			std::array<const int32_t*, kTracks> in{};
			std::array<std::array<uint32_t, 5>, kTracks> mix{};
			for(int t = 0; t < kTracks; ++t)
			{
				const auto& mi = m_host->mixerInput(t);
				TrackFx::setParams(m_state[t], mi.fx.data());
				const int32_t* src = m_voiceOut[t].data();
				int32_t* dst = _out.tracks[t].data();
				const bool silentIn = std::all_of(src, src + kBlock, [](int32_t s) { return s == 0; });
				// A silent track whose effect chain has settled (a block of silence left its state unchanged and
				// output nothing) gives the same again for as long as its input stays silent and its state (which
				// holds its params) stays the same: skip it. Exact - see HANDOFF.md, "fixed per-block cost".
				if(skipSettled && silentIn && m_settled[t] && m_state[t].y == m_settledState[t].y)
				{
					std::fill(dst, dst + kBlock, 0);
				}
				else
				{
					const auto before = m_state[t];
					m_fx.process(m_state[t], src, dst);
					m_settled[t] = silentIn && m_state[t].y == before.y && std::all_of(dst, dst + kBlock, [](int32_t s) { return s == 0; });
					if(m_settled[t])
						m_settledState[t] = m_state[t];
				}
				in[t] = dst;
				mix[t] = mi.mix;
			}
			m_mixer.process(in.data(), mix.data(), _out.mix);
			return true;
		}

		const std::string& fault() const { return m_voices->faultReason(); }

		bool skipSettled = true;	// false: run every track's effects every block (for verifying the skip)

	private:
		void init()
		{
			m_host = std::make_unique<HostModel<TVoices>>(*m_os, *m_voices);
			for(auto& s : m_state)
				TrackFx::init(s);
		}

		std::unique_ptr<MachineRunner> m_os;
		std::unique_ptr<TVoices> m_voices;
		std::unique_ptr<HostModel<TVoices>> m_host;
		TrackFx::Tables m_tables;
		TrackFx m_fx;
		Mixer m_mixer;
		std::array<TrackFx::State, kTracks> m_state{};
		std::array<TrackFx::State, kTracks> m_settledState{};
		std::array<bool, kTracks> m_settled{};
		typename TVoices::Block m_voiceOut{};
	};

	using Engine = EngineT<VoiceEngine>;
	// Constructed with (firmware, osImage, groupCount) - see ParallelVoiceEngine.h.
	using ParallelEngine = EngineT<ParallelVoiceEngine>;
}
