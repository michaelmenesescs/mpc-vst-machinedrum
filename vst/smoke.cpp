// Drives mpc_engine() like the host does: 128-frame blocks paced to real time. Prints readiness time, peak
// and underruns. usage: md-vst-smoke <data-dir> [seconds]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
extern "C" {
#include "engine.h"
}
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
	const mpc_engine_t* e = mpc_engine();
	const auto t0 = Clock::now();
	void* in = e->create(argc > 1 ? argv[1] : ".");
	char buf[32];
	// A project restore arrives before the engine has booted: machine first, then a saved SYN value.
	e->set_param(in, "track3_machine", "28");	// TRXB2
	e->set_param(in, "track3_syn2", "5");
	while(true)
	{
		if(e->get_param(in, "ready", buf, sizeof buf) > 0 && buf[0] == '1') break;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if(Clock::now() - t0 > std::chrono::seconds(60)) { puts("engine never became ready"); return 1; }
	}
	printf("ready after %.0f ms\n", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

	// machine 0 (GND--) is silent by design; assign real machines to the 3 tracks this test triggers
	// (ids from the OS's own descriptor table, not guaranteed stable across OS versions - fine for a smoke
	// test, not something a real preset should hardcode).
	e->set_param(in, "track0_machine", "16");	// TRXBD
	e->set_param(in, "track1_machine", "17");	// TRXSD
	e->set_param(in, "track2_machine", "22");	// TRXCH

	// SYN1-8: untouched knobs follow the machine's defaults and labels; a touched one keeps its value.
	auto dumpSyn = [&](int t)
	{
		printf("track%d syn:", t);
		for(int p = 1; p <= 8; ++p)
		{
			char k[32], v[16] = "?", n[16] = "?";
			snprintf(k, sizeof k, "track%d_syn%d", t, p);
			e->get_param(in, k, v, sizeof v);
			snprintf(k, sizeof k, "track%d_syn%d_name", t, p);
			e->get_param(in, k, n, sizeof n);
			printf(" %s=%s", n, v);
		}
		printf("\n");
	};
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	dumpSyn(0);
	dumpSyn(3);	// expect TRXB2's defaults except DEC=5
	e->set_param(in, "track1_syn1", "99");
	e->set_param(in, "track1_machine", "28");
	dumpSyn(1);	// machine change after the touch: all defaults
	e->set_param(in, "track1_syn1", "99");
	dumpSyn(1);	// PTCH=99
	e->set_param(in, "track1_machine", "17");

	const int secs = argc > 2 ? std::atoi(argv[2]) : 8;
	static int16_t out[128 * 2];
	int peak = 0;
	const uint8_t kick[3] = {0x90, 36, 120}, snare[3] = {0x90, 37, 100}, hat[3] = {0x90, 38, 90};
	const double cpu0 = static_cast<double>(clock());
	const auto t1 = Clock::now();
	auto next = t1;
	const int blocks = secs * 44100 / 128;
	for(int b = 0; b < blocks; ++b)
	{
		if(b % 12 == 0) e->midi(in, kick, 3);
		if(b % 24 == 12) e->midi(in, snare, 3);
		if(b % 6 == 3) e->midi(in, hat, 3);
		e->render(in, out, 128);
		for(int i = 0; i < 128 * 2; ++i) if(std::abs(out[i]) > peak) peak = std::abs(out[i]);
		next += std::chrono::microseconds(2902);
		std::this_thread::sleep_until(next);
	}
	e->get_param(in, "underruns", buf, sizeof buf);
	printf("%.1f s: peak %d/32767, underruns %s, wall cpu %.0f ms\n", double(secs), peak, buf,
		(static_cast<double>(clock()) - cpu0) / CLOCKS_PER_SEC * 1000.0);
	e->destroy(in);
	return 0;
}
