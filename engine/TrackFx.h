// The Machinedrum's per-track effect chain (AMD, EQ, filter, SRR, distortion) as native C++, translated from
// the mixer DSP's (DSP1) own code and bit-exact with it: same fixed-point arithmetic (Dsp56.h), same per-track
// state layout. Verified sample by sample against the DSP code running in the emulator (tools/mdmix).
// The lookup tables come from the user's OS file at load time; none are built into the code.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "../tools/mdfw/Firmware.h"

namespace md::engine
{
	class TrackFx
	{
	public:
		static constexpr int kBlock = 32;

		// Parameter words and state for one track: the DSP's Y:$200+$40k block. Words 0-8 = AMD AMF EQF EQG
		// FLTF FLTW FLTQ SRR DIST (value << 7); the rest is filter/oscillator state.
		struct State
		{
			std::array<int32_t, 64> y{};
		};

		// The DSP's external memory $140000-$14ffff: the OS file's tables, plus the sine table that the DSP
		// computes at boot ($148000-$14ffff).
		class Tables
		{
		public:
			explicit Tables(const fw::Firmware& _fw);
			int32_t operator[](const uint32_t _addr) const { return m_ext[(_addr - kBase) & 0xffff]; }
			static constexpr uint32_t kBase = 0x140000;
		private:
			std::vector<int32_t> m_ext;
		};

		explicit TrackFx(const Tables& _tables) : m_t(_tables) {}

		static void init(State& _s);	// as the DSP's boot ($100031)
		static void setParams(State& _s, const uint32_t* _fx9);

		// One block: _in and _out are 24-bit samples (sign-extended).
		void process(State& _s, const int32_t* _in, int32_t* _out);

		// Scratch as the DSP's X:$000-$0ff, for stage-by-stage comparison with the DSP.
		const std::array<int32_t, 256>& scratch() const { return m_x; }
		void resetScratch() { m_x.fill(0); }

		enum class Stop { None, AfterAmd, AfterEq };
		Stop stopAfter = Stop::None;

	private:
		void prepFilter(State& _s);
		void amd(State& _s, const int32_t* _in);
		void eq(State& _s);

		const Tables& m_t;
		std::array<int32_t, 256> m_x{};
	};
}
