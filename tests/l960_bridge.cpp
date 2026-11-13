// Development tool: runs the emulated LARC2 and reverb card behind HostSystem and waits for the original
// host software (helper process) to connect; optionally launches it (L960_LAUNCH=1 uses
// HostSystem::launchHelper, i.e. L960_HELPER_CMD on non-Windows systems).
// At the end it reports what the host programmed into the Lexichips and runs an impulse through machine A.
// Usage: l960_bridge <960L folder> [seconds] [lcd.ppm] [data dir]
#include "l960/engine.h"
#include "l960/hostsystem.h"
#include "l960/larc2/larc2device.h"
#include "testutil.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

int main(int argc, char** argv)
{
	const auto root = firmwareDir(argc, argv);
	const int seconds = argc > 2 ? std::atoi(argv[2]) : 120;
	const std::string ppm = argc > 3 ? argv[3] : "bridge_lcd.ppm";
	const std::filesystem::path dataDir = argc > 4 ? argv[4] : "l960_data";
	setvbuf(stdout, nullptr, _IOLBF, 0);

	l960::larc2::Larc2Device dev;
	std::string err;
	L960_CHECK(dev.load(root, &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));

	l960::Engine engine;
	engine.setMode(l960::Engine::Mode::HostDriven);
	l960::HostSystem host(engine.card(), engine.cardLock(), dev);
	std::mutex logLock;
	host.onLog = [&](const std::string& s) { std::lock_guard<std::mutex> l(logLock); std::printf("%s\n", s.c_str()); };
	L960_CHECK(host.start(root, &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));
	dev.start();

	// Keep the audio side running like a plugin would (silence in), so Lexichip state is exercised concurrently
	std::atomic<bool> quit{false};
	std::thread audio([&] {
		std::vector<float> in(256, 0.0f), o[8];
		for (auto& v : o) v.resize(256);
		const float* ins[2] = {in.data(), in.data()};
		float* outs[8];
		for (int i = 0; i < 8; ++i) outs[i] = o[i].data();
		while (!quit)
		{
			engine.process(ins, 2, outs, 8, 256);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	});

	// L960_SCRIPT="70:touch 0 1;71:fader 0 0.2;73:key F1;80:dump name" -- actions at seconds since start
	struct Action { int t; std::string verb, a, b; };
	std::vector<Action> script;
	if (const char* sc = std::getenv("L960_SCRIPT"))
	{
		std::string all = sc, item;
		std::stringstream ss(all);
		while (std::getline(ss, item, ';'))
		{
			Action a{};
			std::stringstream is(item);
			char colon;
			is >> a.t >> colon >> a.verb >> a.a >> a.b;
			script.push_back(a);
		}
	}
	auto dumpLcd = [&](const std::string& _file) {
		std::vector<uint8_t> rgb;
		uint32_t w, h;
		dev.lcd(rgb, w, h);
		std::ofstream f(_file, std::ios::binary);
		f << "P6\n" << w << " " << h << "\n255\n";
		f.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
	};
	bool launched = false;
	for (int t = 0; t < seconds; ++t)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (!launched && std::getenv("L960_LAUNCH") && dev.linked())
		{
			launched = true;
			if (!host.launchHelper(root, dataDir, &err)) std::printf("launch failed: %s\n", err.c_str());
		}
		for (const auto& a : script)
		{
			if (a.t != t + 1) continue;
			std::printf("script t=%d: %s %s %s\n", a.t, a.verb.c_str(), a.a.c_str(), a.b.c_str());
			if (a.verb == "touch") dev.setFaderTouched(std::stoi(a.a), a.b == "1");
			else if (a.verb == "fader") dev.setFader(std::stoi(a.a), std::stof(a.b));
			else if (a.verb == "key") { dev.pressKey(a.a, true); std::this_thread::sleep_for(std::chrono::milliseconds(150)); dev.pressKey(a.a, false); }
			else if (a.verb == "joy") dev.setJoystick(std::stof(a.a), std::stof(a.b));
			else if (a.verb == "dump") dumpLcd(a.a);
			else if (a.verb == "ad") dev.adOverride(std::stoi(a.a), std::stoi(a.b));
			else if (a.verb == "fpga") std::printf("FPGA trace:\n%s", dev.fpgaTrace().c_str());
		}
		if (t % 5 == 4)
		{
			std::printf("t=%ds  LARC2: %s  |  %s\n", t + 1, dev.status().c_str(), host.status().c_str());
			std::vector<uint8_t> rgb;
			uint32_t w, h;
			dev.lcd(rgb, w, h);
			std::ofstream f(ppm, std::ios::binary);
			f << "P6\n" << w << " " << h << "\n255\n";
			f.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
		}
	}
	quit = true;
	audio.join();

	for (int i = 0; i < 4; ++i)
	{
		std::lock_guard<std::mutex> l(engine.cardLock());
		auto& lx = engine.card().lexichip(i);
		int wcs = 0;
		for (uint16_t a = 0; a < 0x500; ++a) if (lx.busRead(uint16_t(0x600 + a))) ++wcs;
		std::printf("Lexichip %d: %d nonzero WCS bytes, program length %u\n", i, wcs, lx.programLength());
		if (const char* d = std::getenv("L960_DUMP_LEX"))
		{
			std::ofstream f(std::string(d) + std::to_string(i) + ".bin", std::ios::binary);
			for (uint16_t a = 0; a < l960::Lexichip::BusSize; ++a) f.put(char(lx.busRead(a)));
		}
	}
	{
		auto& card = engine.card();
		auto row = [&](int t, int port) {
			std::printf("T-MIX%d port %2d:", t + 1, port);
			for (int c = 0; c < 8; ++c) std::printf(" %02X", card.tmix(t).reg(uint8_t(0xA0 + 8 * port + c)));
			std::printf("   dir $%02X\n", card.tmix(t).reg(uint8_t(0x48 + port)));
		};
		row(0, 10); row(0, 11); row(2, 0); row(2, 1); row(2, 4); row(2, 5); row(1, 8); row(1, 9);
		int nz = 0;
		for (uint32_t i = 0; i < 0x108; ++i) if (card.dsp().readX(0x100 + i)) ++nz;
		std::printf("mixer targets: %d nonzero in X:$100-$207, Gret Y:$150 = %06X\n", nz, card.dsp().readY(0x150));
	}
	{
		// impulse through machine A
		constexpr int N = 48000;
		std::vector<float> in(N, 0.0f), o[8];
		in[0] = 0.5f;
		for (auto& v : o) v.resize(N);
		const float* ins[2] = {in.data(), in.data()};
		float* outs[8];
		for (int i = 0; i < 8; ++i) outs[i] = o[i].data();
		{
			std::vector<float> dc(64, 0.25f), junk(64);
			const float* d2[2] = {dc.data(), dc.data()};
			float* j8[8];
			for (auto& p : j8) p = junk.data();
			engine.process(d2, 2, j8, 8, 64);
			std::printf("DC test: Lexichip 1 reg $52=%d $54=%d, T-MIX1 $380=%06X, T-MIX3 $390=%06X\n", engine.card().lexichip(0).reg(0x52),
			            engine.card().lexichip(0).reg(0x54), engine.card().tmix(0).dspRead(0x380), engine.card().tmix(2).dspRead(0x390));
		}
		int32_t peak52 = 0;
		for (int k = 0; k < N; k += 64)
		{
			const float* in2[2] = {in.data() + k, in.data() + k};
			float* o2[8];
			for (int i = 0; i < 8; ++i) o2[i] = o[i].data() + k;
			engine.process(in2, 2, o2, 8, 64);
			if (k == 0) peak52 = engine.card().lexichip(0).reg(0x52);
		}
		std::printf("Lexichip 1 reg $52 after first block: %d\n", peak52);
		for (size_t c = 0; c < engine.io().size(); ++c)
			std::printf("chip %zu: %zu input slots, %zu output slots\n", c, engine.io()[c].inSlots.size(), engine.io()[c].outSlots.size());
		for (int c = 0; c < 8; ++c)
		{
			double e = 0;
			int last = 0;
			for (int n = 0; n < N; ++n) { e += double(o[c][n]) * o[c][n]; if (std::fabs(o[c][n]) > 1e-5f) last = n; }
			if (e > 0) std::printf("output %d: energy %.4g, tail to %.2f s\n", c, e, last / 48000.0);
		}
	}
	host.stopHelper();
	dev.stop();
	host.stop();
	return 0;
}
