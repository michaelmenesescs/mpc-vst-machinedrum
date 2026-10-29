// Machinedrum One's engine: the OS's own control code (HostModel + MachineRunner), the voice DSP (VoiceEngine,
// emulated) and the mixer DSP's per-track effects and mix as native C++ (TrackFx, Mixer). One 32-sample block
// at a time: all 16 tracks, the dry main mix, the reverb/delay sends and each track's own output.
#pragma once
#include <algorithm>
#include <chrono>
#include <array>
#include <memory>
#include <vector>

#include <type_traits>
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
			: m_tables(_fw), m_mixer(m_tables)
		{
			m_os = std::make_unique<MachineRunner>(std::move(_osImage));
			m_voices = std::make_unique<TVoices>(_fw);
			init();
		}

		// As above, but forwards _voicesArg to TVoices' constructor too (e.g. ParallelVoiceEngine's group
		// count). Only ever instantiated for a TVoices whose constructor actually takes (firmware, _voicesArg).
		template<class TVoicesArg>
		EngineT(const fw::Firmware& _fw, std::vector<uint8_t> _osImage, TVoicesArg _voicesArg)
			: m_tables(_fw), m_mixer(m_tables)
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

		// Optional stage timing (microseconds, accumulated): track effects, and the mix. The OS tick and the voice DSP
		// are HostModel's (host().tickUs / dspUs).
		uint32_t dryMute = 0;	// tracks kept out of the dry main mix (see Mixer::process); set by the caller between renders
		bool timingOn = false;
		double fxUs = 0, mixUs = 0;

		static constexpr bool kParallel = std::is_same_v<TVoices, ParallelVoiceEngine>;

		// One track's effects: a settled silent track's chain gives the same again while its input stays silent and
		// its state (which holds its params) stays the same, so it is skipped. Exact - see HANDOFF.md, "fixed
		// per-block cost". Touches only track _t's state, so tracks can run on different threads (each with its own
		// TrackFx: its scratch buffers are per instance).
		void fxTrack(TrackFx& _fx, const int _t, const int32_t* _src, int32_t* _dst)
		{
			TrackFx::setParams(m_state[_t], m_host->mixerInput(_t).fx.data());
			const bool silentIn = std::all_of(_src, _src + kBlock, [](int32_t s) { return s == 0; });
			if(skipSettled && silentIn && m_settled[_t] && m_state[_t].y == m_settledState[_t].y)
			{
				std::fill(_dst, _dst + kBlock, 0);
				return;
			}
			const auto before = m_state[_t];
			_fx.process(m_state[_t], _src, _dst);
			m_settled[_t] = silentIn && m_state[_t].y == before.y && std::all_of(_dst, _dst + kBlock, [](int32_t s) { return s == 0; });
			if(m_settled[_t])
				m_settledState[_t] = m_state[_t];
		}

		bool render(Output& _out)
		{
			m_host->timingOn = timingOn;
			m_out = &_out;
			if(!m_host->renderBlock(m_voiceOut))
				return false;
			const auto tf0 = timingOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			std::array<const int32_t*, kTracks> in{};
			std::array<std::array<uint32_t, 5>, kTracks> mix{};
			for(int t = 0; t < kTracks; ++t)
			{
				// Parallel: the groups' threads already ran their tracks' effects (see init()).
				if constexpr(!kParallel)
					fxTrack(*m_fxs[0], t, m_voiceOut[t].data(), _out.tracks[t].data());
				in[t] = _out.tracks[t].data();
				mix[t] = m_host->mixerInput(t).mix;
			}
			if(!timingOn) { m_mixer.process(in.data(), mix.data(), _out.mix, dryMute); return true; }
			const auto tf1 = std::chrono::steady_clock::now();
			m_mixer.process(in.data(), mix.data(), _out.mix, dryMute);
			const auto tf2 = std::chrono::steady_clock::now();
			fxUs += std::chrono::duration<double, std::micro>(tf1 - tf0).count();
			mixUs += std::chrono::duration<double, std::micro>(tf2 - tf1).count();
			return true;
		}

		const std::string& fault() const { return m_voices->faultReason(); }

		// The machines warmUpVoices runs by default: every audio machine (not MID/CTR) except RAM-R/RAM-P (they record and
		// play the input), and the ROM machines only if their samples are loaded (_rom).
		std::vector<uint8_t> warmableMachines(const bool _rom) const
		{
			std::vector<uint8_t> ids;
			for(const auto& m : m_os->machines())
			{
				const bool rom = (m.id >= 128 && m.id < 160) || (m.id >= 176 && m.id < 192);
				if(m.name.empty() || (m.id >= 0x60 && m.id <= 0x7b) || m.name.rfind("RAM", 0) == 0 || (rom && !_rom))
					continue;
				ids.push_back(m.id);
			}
			return ids;
		}

		// Warm-up adapted from c0remusic/MModule (MdEngine.h, EngineT::warmUpVoices, GPL-3.0). The voice DSP's JIT compiles
		// a machine's code the first time a voice runs it, a block or more of DSP time at once: the startup and machine-change
		// underruns. Here each machine of _machines plays a Hit at its default SYN1-8 for _blocks blocks, 16 machines at a
		// time across the 16 voice slots, their words computed by a copy of the OS; every voice DSP (every group of a
		// ParallelVoiceEngine) then gets its memory and registers back (VoiceEngine::saveState), so the engine sounds as if
		// nothing had run. Call after the ROM samples are written, before the first render. Returns the machines run, or -1
		// if a voice DSP faulted.
		int warmUpVoices(const std::vector<uint8_t>& _machines, const int _blocks = 4)
		{
			using WarmBlock = typename TVoices::WarmBlock;
			const auto os = m_os->clone();
			std::vector<WarmBlock> script;
			for(size_t first = 0; first < _machines.size(); first += kTracks)
			{
				for(int b = 0; b < _blocks; ++b)
				{
					WarmBlock blk{};
					for(size_t v = 0; v < static_cast<size_t>(kTracks) && first + v < _machines.size(); ++v)
					{
						const auto* m = os->machine(_machines[first + v]);
						if(!m)
							continue;
						std::array<uint16_t, 24> params{};	// the per-voice array; SYN1-8 only: the machine reads no other
						for(size_t p = 0; p < 8; ++p)
							params[p] = static_cast<uint16_t>(m->defaults[p] << 7);
						uint32_t out[32];
						const int n = os->compute(m->id, params.data(), out, 32, b == 0);
						if(n < 0)
							continue;
						out[0] = b == 0 ? static_cast<uint32_t>(m->id) + 1 : 0;	// the trigger word, as HostModel::updateVoice
						auto& slot = blk[v];
						slot.count = std::clamp(n, 1, static_cast<int>(TVoices::kSlotWords));
						std::copy(out, out + slot.count, slot.words.begin());
					}
					script.push_back(blk);
				}
			}
			return m_voices->warmUp(script) ? static_cast<int>(_machines.size()) : -1;
		}

		bool skipSettled = true;	// false: run every track's effects every block (for verifying the skip)

	private:
		void init()
		{
			m_host = std::make_unique<HostModel<TVoices>>(*m_os, *m_voices);
			for(auto& s : m_state)
				TrackFx::init(s);
			m_fxs.push_back(std::make_unique<TrackFx>(m_tables));
			if constexpr(kParallel)
			{
				// Each group's thread runs the effects of its own tracks right after its voices render.
				for(int g = 1; g < m_voices->groupCount(); ++g)
					m_fxs.push_back(std::make_unique<TrackFx>(m_tables));
				m_voices->setPost([this](int _g, const typename TVoices::Block& _blk)
				{
					for(int t = 0; t < kTracks; ++t)
						if(m_voices->groupOf(t) == _g)
							fxTrack(*m_fxs[static_cast<size_t>(_g)], t, _blk[t].data(), m_out->tracks[t].data());
				});
			}
		}

		std::unique_ptr<MachineRunner> m_os;
		std::unique_ptr<TVoices> m_voices;
		std::unique_ptr<HostModel<TVoices>> m_host;
		TrackFx::Tables m_tables;
		std::vector<std::unique_ptr<TrackFx>> m_fxs;	// one per voice group (one when not parallel)
		Output* m_out = nullptr;
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
