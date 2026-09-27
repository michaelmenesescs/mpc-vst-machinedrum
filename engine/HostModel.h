// The Machinedrum's main-processor side, for the voice DSP: per-track kit parameters, machine assignment,
// triggers and the control tick. The maths is the OS's own code run in MachineRunner (parameter smoothing
// $10002e0, LFO oscillator $1000088, LFO apply $1000332, machine coefficient functions); this class does what
// the OS's tick routine ($20ad9a) and slot pump ($10004d0) do around them. See docs/PROTOCOL.md.
//
// Not yet modelled: parameter locks and trigger groups (sequencer features), the mixer DSP's per-track values.
#pragma once
#include <array>
#include <cstdint>

#include "MachineRunner.h"
#include "VoiceEngine.h"

namespace md::engine
{
	class HostModel
	{
	public:
		static constexpr int kTracks = 16;
		static constexpr int kParams = 24;	// SYN1-8, AMD AMF EQF EQG FLTF FLTW FLTQ SRR DIST, VOL PAN DEL REV, LFOS LFOD LFOM

		HostModel(MachineRunner& _os, VoiceEngine& _voices);

		// Assign a machine (0-191, MachineInfo::id). SYN1-8 take the machine's defaults. As on the MD, the voice
		// switches machine at its next trigger, where all 24 parameters take effect immediately (no glide).
		void setMachine(int _track, uint8_t _machineId);
		void setParam(int _track, int _param, int _value);	// 0-127, smoothed like the MD
		int param(int _track, int _param) const { return m_raw[_track][_param]; }
		void setTempo(double _bpm);							// tempo factor for tempo-synced LFOs and E12/ROM retrig
		void trigger(int _track);

		// A track's LFO (LFO k belongs to track k; its speed, depth and mix are track parameters 21-23):
		// destination track and parameter (0-23), two shapes, type (bit 0 TRIG: restart on the track's trigger,
		// bit 1 HOLD: output only updated at a trigger; 0 = FREE).
		void setLfo(int _track, int _destTrack, int _destParam, int _shape1, int _shape2, int _type);

		// Control ticks: the MD runs its tick as fast as the ColdFire gets through it (~120 Hz measured in
		// gearmulator-md-mm, varying with load). Default: one tick every 11 blocks of 32 samples (125.3 Hz).
		void setBlocksPerTick(int _n) { m_blocksPerTick = _n; }

		// Render one 32-sample block of the 16 voices (runs a tick first when due).
		bool renderBlock(VoiceEngine::Block& _out);
		void tick();
		void updateVoice(int _track);	// machine function on the voice's current array -> voice slot

		const uint16_t* voiceParams(int _track) const;	// the per-voice 24-value array after smoothing and LFO

	private:
		MachineRunner& m_os;
		VoiceEngine& m_voices;
		std::array<std::array<uint8_t, kParams>, kTracks> m_raw{};
		std::array<uint8_t, kTracks> m_machine{};
		std::array<int, kTracks> m_pendingMachine{};	// -1 = none
		std::array<bool, kTracks> m_trigger{};
		std::array<uint16_t, kParams> m_scratch{};
		int m_blocksPerTick = 11;
		int m_blockCount = 0;
		uint32_t m_tickCount = 0;
	};
}
