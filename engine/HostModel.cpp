#include "HostModel.h"

#include <algorithm>
#include <cmath>

namespace md::engine
{
	namespace
	{
		// OS 1.63 addresses (internal SRAM $1000000 = the OS's copy of image $2622f4)
		constexpr uint32_t kRaw = 0x1000ddc;		// smoothing targets: 16 tracks x 24 bytes (0-127)
		constexpr uint32_t kWork = 0x1000a4c;		// smoothed values: 16 x 24 words (value << 7, up to $3fff)
		constexpr uint32_t kVoiceParams = 0x10011cc;	// per-voice arrays (a6): 16 x 24 words, after LFO
		constexpr uint32_t kTempo = 0x100150c;		// BPM x 24
		constexpr uint32_t kSmooth = 0x10002e0, kLfoOsc = 0x1000088, kLfoApply = 0x1000332;
		constexpr uint32_t kLfo = 0x1000f8c, kLfoStride = 0x24;	// 16 LFO structs: bytes 0-4 settings, 5 = trigger flag
		constexpr uint32_t kLfoWave = 0x204c94;		// waveform/restart for one LFO (in the tick routine's trigger path)
		constexpr uint32_t kLfoApplyOne = 0x10001e8;	// apply one LFO to its destination in the voice arrays
		constexpr uint32_t kTrackStride = 0x30;

		bool isAudioMachine(const uint8_t _id) { return !(_id >= 0x60 && _id <= 0x7b); }	// MID/CTR: no audio
	}

	HostModel::HostModel(MachineRunner& _os, VoiceEngine& _voices) : m_os(_os), m_voices(_voices)
	{
		setTempo(125.0);
		m_pendingMachine.fill(-1);
		for(int t = 0; t < kTracks; ++t)
		{
			m_machine[t] = 0;
			setMachine(t, 0);
			setLfo(t, t, 0, 0, 0, 0);
		}
	}

	void HostModel::setMachine(const int _track, const uint8_t _machineId)
	{
		const auto* m = m_os.machine(_machineId);
		if(!m) return;
		m_pendingMachine[_track] = _machineId;
		for(int p = 0; p < 8; ++p)
			m_raw[_track][p] = m->defaults[p];
	}

	void HostModel::setParam(const int _track, const int _param, const int _value)
	{
		m_raw[_track][_param] = static_cast<uint8_t>(std::clamp(_value, 0, 127));
	}

	void HostModel::setTempo(const double _bpm)
	{
		m_os.poke32(kTempo, static_cast<uint32_t>(std::lround(std::clamp(_bpm, 30.0, 300.0) * 24.0)));
	}

	void HostModel::setLfo(const int _track, const int _destTrack, const int _destParam, const int _shape1, const int _shape2, const int _type)
	{
		const uint32_t base = kLfo + kLfoStride * static_cast<uint32_t>(_track);
		m_os.poke8(base + 0, static_cast<uint8_t>(std::clamp(_destTrack, 0, kTracks - 1)));
		m_os.poke8(base + 1, static_cast<uint8_t>(std::clamp(_destParam, 0, kParams - 1)));
		m_os.poke8(base + 2, static_cast<uint8_t>(std::clamp(_shape1, 0, 7)));
		m_os.poke8(base + 3, static_cast<uint8_t>(std::clamp(_shape2, 0, 7)));
		m_os.poke8(base + 4, static_cast<uint8_t>(std::clamp(_type, 0, 3)));
	}

	void HostModel::trigger(const int _track)
	{
		m_trigger[_track] = true;
		// The OS's track trigger ($20cdf0) flags the track's own LFO; the tick's trigger path acts on it.
		m_os.poke8(kLfo + kLfoStride * static_cast<uint32_t>(_track) + 5, 1);
		// The tick routine's trigger path: a pending machine is applied now, loading the kit values straight into
		// the smoothing targets, the smoothed array and the voice array (no glide).
		if(m_pendingMachine[_track] >= 0)
		{
			m_machine[_track] = static_cast<uint8_t>(m_pendingMachine[_track]);
			m_pendingMachine[_track] = -1;
			for(int p = 0; p < kParams; ++p)
			{
				const auto v = static_cast<uint16_t>(m_raw[_track][p] << 7);
				m_os.poke8(kRaw + 24 * static_cast<uint32_t>(_track) + static_cast<uint32_t>(p), m_raw[_track][p]);
				m_os.poke16(kWork + kTrackStride * static_cast<uint32_t>(_track) + 2 * static_cast<uint32_t>(p), v);
				m_os.poke16(kVoiceParams + kTrackStride * static_cast<uint32_t>(_track) + 2 * static_cast<uint32_t>(p), v);
			}
		}
	}

	const uint16_t* HostModel::voiceParams(const int _track) const
	{
		auto* self = const_cast<HostModel*>(this);
		for(int p = 0; p < kParams; ++p)
			self->m_scratch[p] = m_os.peek16(kVoiceParams + kTrackStride * static_cast<uint32_t>(_track) + 2 * static_cast<uint32_t>(p));
		return m_scratch.data();
	}

	void HostModel::updateVoice(const int _track)
	{
		// As the tick routine's voice loop: the machine function on the voice's current array; word 0 = trigger
		// flag, which the slot pump turns into machine id + 1. MID/CTR machines have no audio and are not sent.
		// The tick routine's trigger path, before the machine function: the LFO's restart/hold for this
		// trigger, then this track's LFO applied to its destination straight away.
		if(m_trigger[_track])
		{
			m_os.call(kLfoWave, {static_cast<uint32_t>(_track)});
			m_os.call(kLfoApplyOne, {static_cast<uint32_t>(_track)});
		}
		const auto id = m_machine[_track];
		if(isAudioMachine(id))
		{
			uint32_t out[32];
			const int n = m_os.compute(id, voiceParams(_track), out, 32);
			if(n > 0)
			{
				out[0] = m_trigger[_track] ? static_cast<uint32_t>(id) + 1 : 0;
				m_voices.setSlot(_track, out, std::min(n, VoiceEngine::kSlotWords));
			}
		}
		m_trigger[_track] = false;
	}

	void HostModel::tick()
	{
		for(int t = 0; t < kTracks; ++t)
			for(int p = 0; p < kParams; ++p)
				m_os.poke8(kRaw + 24 * static_cast<uint32_t>(t) + static_cast<uint32_t>(p), m_raw[t][p]);

		for(int t = 0; t < kTracks; ++t)
			updateVoice(t);

		// After the voice loop the OS smooths the parameters, runs the LFOs and builds the next tick's arrays.
		m_os.call(kSmooth, {});
		m_os.call(kLfoOsc, {});
		m_os.call(kLfoApply, {});
		++m_tickCount;
	}

	bool HostModel::renderBlock(VoiceEngine::Block& _out)
	{
		// Ticks on a fixed block schedule; a trigger between ticks updates just its voice, so it starts on this
		// block rather than waiting for the next tick.
		if(m_blockCount++ % m_blocksPerTick == 0)
			tick();
		else
			for(int t = 0; t < kTracks; ++t)
				if(m_trigger[t])
					updateVoice(t);
		return m_voices.renderBlock(_out);
	}
}
