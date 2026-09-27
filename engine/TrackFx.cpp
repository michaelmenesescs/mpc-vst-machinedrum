#include "TrackFx.h"

#include "Dsp56.h"

namespace md::engine
{
	using namespace d56;

	namespace
	{
		constexpr uint32_t kSine = 0x148000;	// 32768 words, one sine period

		int32_t lim48hi(const Acc _a, uint32_t& _lo)
		{
			if(_a > 0x7fffffffffffLL) { _lo = 0xffffff; return 0x7fffff; }
			if(_a < -0x800000000000LL) { _lo = 0; return -0x800000; }
			_lo = a0(_a);
			return a1(_a);
		}
	}

	TrackFx::Tables::Tables(const fw::Firmware& _fw) : m_ext(0x10000, 0)
	{
		for(const auto& r : _fw.dspB.records)
			for(size_t k = 0; k < r.words.size(); ++k)
			{
				const uint32_t a = r.addr + static_cast<uint32_t>(k);
				if(a >= kBase && a < kBase + 0x10000)
					m_ext[a - kBase] = sx24(r.words[k]);
			}

		// The boot code's sine: s[n+1] = 2c s[n] - s[n-1] in 48-bit double precision, c = cos(2 pi / 32768).
		int32_t hi[2] = {0, 0x648};
		uint32_t lo[2] = {0, 0x7ed46b};
		const int32_t cHi = 0x7fffff, cLo = sx24(0xd88586);
		uint32_t out = kSine - kBase;
		m_ext[out++] = 0;
		m_ext[out++] = 0x648;
		int r = 1;
		for(int n = 0; n < 0x7ffe; ++n)
		{
			const int32_t y1 = hi[r];
			const int32_t y0 = sx24(lo[r]);
			r ^= 1;
			Acc a = mpyuu(cLo, y0);
			a = dmac(a, mpysu(cHi, y0));
			a = add(a, mpysu(y1, cLo));
			a = dmac(a, mpy(cHi, y1));
			a = asl(a, 1);
			a = sub(a, acc48(hi[r], lo[r]));
			hi[r] = lim48hi(a, lo[r]);
			m_ext[out++] = lim(a);
		}
	}

	void TrackFx::init(State& _s)
	{
		_s.y.fill(0);
		_s.y[5] = 0x3fff;
		_s.y[1] = _s.y[2] = _s.y[3] = 0x2000;
		_s.y[0x25] = kSine;
	}

	void TrackFx::setParams(State& _s, const uint32_t* _fx9)
	{
		for(int k = 0; k < 9; ++k)
			_s.y[k] = sx24(_fx9[k]);
	}

	// P:$a4-$ba. Filter cutoffs and resonances from FLTF, FLTW, FLTQ.
	void TrackFx::prepFilter(State& _s)
	{
		auto& y = _s.y;
		Acc b = asr(acc(y[6]), 7);				// FLTQ
		Acc a = asr(acc(y[4]), 3);				// FLTF << 4
		const int32_t q = lim(b);
		if(a == 0) b = 0;
		y[0x22] = lim(a);
		y[0x23] = lim(b);
		b = asr(acc(y[5]), 3);					// FLTW << 4
		a = add(a, b);
		if(a > acc(0x7ff)) a = acc(0x7ff);
		y[0x24] = lim(a);
		a = acc(q);
		if(b >= acc(0x7ef)) a = 0;
		y[0x26] = lim(a);
	}

	// P:$bb-$d1. Amplitude modulation by the sine table: out = (1 - d) in + d (in x sin), d = AMD, rate AMF.
	// Output at X:$c-$2b (the DSP's X:$b gets a stale word).
	void TrackFx::amd(State& _s, const int32_t* _in)
	{
		auto& y = _s.y;
		auto& x = m_x;
		const int32_t n1 = lim(asr(acc(y[1]), 3));	// AMF << 4
		const Acc b0 = asl(acc(y[0]), 9);			// AMD << 16
		uint32_t r1 = static_cast<uint32_t>(y[0x25]);
		auto sine = [&]() { const int32_t v = m_t[r1]; r1 = modAdd(r1, n1, 0x7fff); return v; };

		const Acc aDepth = add(accShortImm(0x80), b0);
		const int32_t dMinus1 = lim(aDepth);		// x1 = d - 1
		const int32_t d = lim(b0);					// y1
		x[0xb] = lim(b0);
		int32_t s = sine();
		for(int j = 0; j < kBlock; ++j)
		{
			const int32_t ringed = lim(mpy(_in[j], s));
			s = sine();
			x[0xc + j] = lim(add(mpy(-dMinus1, _in[j]), mpy(d, ringed)));
		}
		r1 = modAdd(r1, -n1, 0x7fff);
		y[0x25] = static_cast<int32_t>(r1);
	}

	// P:$d2-$133. EQ: a peaking biquad (EQF frequency, EQG gain) on the AMD output. The coefficients come from
	// the OS tables indexed by EQG and EQF; the gain normalisation uses the DSP's 24-step division. The filter
	// runs in place on X:$8-$2b (history in X:$8-$b and state words $12-$16).
	void TrackFx::eq(State& _s)
	{
		auto& y = _s.y;
		auto& x = m_x;
		std::array<int32_t, 5> c{};		// Y:$50-$54, addressed by r5 modulo 5
		int r5 = 0;
		auto pushC = [&](const int32_t _v) { c[r5] = _v; r5 = (r5 + 1) % 5; };
		auto popC = [&]() { const int32_t v = c[r5]; r5 = (r5 + 1) % 5; return v; };

		Acc b = asr(acc(y[3]), 7);										// EQG
		Acc a = acc(y[2]);
		uint32_t r2 = u24(lim(b));
		a = asr(a, 3);													// EQF << 4
		int32_t x0 = m_t[r2 + 0x143476];
		int32_t x1 = m_t[r2 + 0x143576];
		{ const Acc oa = a; a = add(a, acc(x0)); b = acc(a1(oa)); }
		uint32_t r0 = 8;
		b = add(b, acc(x1));
		uint32_t r1 = u24(a1(a));
		int32_t y1 = y[0x16];
		uint32_t r3 = u24(a1(b));
		x[r0++] = y1;
		y1 = y[0x14];
		x[r0++] = y1;
		x0 = m_t[r1 + 0x14221d];
		x1 = m_t[r3 + 0x142c76];
		{ const int32_t ox1 = x1; a = mpy(x1, x0); x0 = ox1; }
		b = wrap(-mpy(x1, x0));
		a = sub(a, acc(0x400000));
		{ const Acc ob = b; b = asr(b, 1); pushC(lim(ob)); }
		{ const Acc oa = a; b = add(b, a); pushC(lim(oa)); }
		b = wrap(-b);
		a = asr(acc(y[2]), 3);
		int32_t y0 = lim(b);
		x0 = m_t[r2 + 0x1434f6];
		x1 = m_t[r2 + 0x1435f6];
		{ const Acc oa = a; a = add(a, acc(x0)); b = acc(a1(oa)); }
		y1 = y[0x13];
		b = add(b, acc(x1));
		r2 = u24(a1(a));
		x[r0++] = y1;
		r3 = u24(a1(b));
		y1 = y[0x12];
		x0 = m_t[r2 + 0x14221d];
		x1 = m_t[r3 + 0x142c76];
		{ const int32_t ox1 = x1; a = wrap(-mpy(x1, x0)); x0 = ox1; }
		b = mpy(x1, x0);
		r1 = 0x80;
		x[r0++] = y1;
		b = asr(b, 1);
		r0 = r1;
		y1 = 0x400000;
		{ const Acc ob = b; b = add(b, acc(y1)); x[r1++] = lim(ob); }
		{ const Acc oa = a; b = add(b, a); x[r1++] = lim(oa); }
		a = acc(clb(b));
		b = normf(a1(a), b);
		x[r1++] = y1;
		{ const Acc ob = b; b = acc(y0); x0 = lim(ob); }
		b = normf(a1(a), b);
		b = asr(b, 3);
		bool carry = false;
		for(int i = 0; i < 24; ++i)
			b = div(b, x0, carry);
		b = acc(sx24(a0(b)));
		b = asl(b, 1);
		x0 = x[r0++];
		y1 = x[0x2b];
		y[0x12] = y1;
		y0 = lim(b);
		{ b = mpy(y0, x0); x0 = x[r0++]; }
		{ a = mpy(y0, x0); x0 = x[r0++]; }
		{ const Acc ob = b; b = mpy(y0, x0); pushC(lim(ob)); }
		pushC(lim(a));
		pushC(lim(b));
		y1 = x[0x2a];
		y[0x13] = y1;

		// the filter loop
		r0 = 8;
		r2 = 9;
		a = acc48(y[0x14], u24(y[0x15]));
		x0 = x[r0++]; y0 = popC();
		a = add(a, mpy(y0, x0)); x1 = x[r0++]; y1 = popC();
		b = acc(x[r2]);
		for(int i = 0; i < kBlock; ++i)
		{
			{ a = add(a, mpy(y1, x1)); x[r2++] = lim(b); }
			{ a = add(a, mpy(y1, x1)); x0 = x[r0++]; y0 = popC(); }
			{ a = add(a, mpy(y0, x0)); x0 = x[r0++]; y0 = popC(); }
			{ a = add(a, mpy(y0, x0)); x0 = x[r0]; r0 -= 3; y0 = popC(); }
			{ a = add(a, mpy(y0, x0)); x0 = x[r0++]; y0 = popC(); }
			{ b = a; x1 = x[r0++]; y1 = popC(); }
			{ const Acc oa = a; a = add(a, mpy(y0, x0)); x1 = lim(oa); }
		}
		x[r2--] = lim(b);
		y[0x14] = lim(b);
		y[0x15] = static_cast<int32_t>(a0(b));
		x0 = x[r2];
		y[0x16] = x0;
	}

	void TrackFx::process(State& _s, const int32_t* _in, int32_t* _out)
	{
		prepFilter(_s);
		amd(_s, _in);
		if(stopAfter == Stop::AfterAmd)
			return;
		eq(_s);
		if(stopAfter == Stop::AfterEq)
			return;
		(void)_out;
	}
}
