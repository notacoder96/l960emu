// Dev tool: run the original host software against the emulated card (like l960_bridge with L960_LAUNCH=1), then
// dump every Lexichip bus image and the Z80 memories (DPRAM + both SRAM windows for all 4 banks).
// Usage: lexi_capture <960L folder> <seconds> <data dir> <output prefix>
#include "l960/engine.h"
#include "l960/hostsystem.h"
#include "l960/larc2/larc2device.h"
#include "testutil.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace l960 { extern FILE* g_lexWriteLog; }

int main(int argc, char** argv)
{
	if (const char* p = std::getenv("LEX_WRITE_LOG")) l960::g_lexWriteLog = std::fopen(p, "w");
	if (argc < 5) { std::puts("usage: lexi_capture <960L> <seconds> <data dir> <out prefix>"); return 1; }
	const std::filesystem::path root = argv[1];
	const int seconds = std::atoi(argv[2]);
	const std::string out = argv[4];
	setvbuf(stdout, nullptr, _IOLBF, 0);

	l960::larc2::Larc2Device dev;
	std::string err;
	L960_CHECK(dev.load(root, &err));
	l960::Engine engine;
	engine.setMode(l960::Engine::Mode::HostDriven);
	l960::HostSystem host(engine.card(), engine.cardLock(), dev);
	L960_CHECK(host.start(root, &err));
	dev.start();
	bool launched = false;
	for (int t = 0; t < seconds; ++t)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (!launched && dev.linked()) { launched = true; if (!host.launchHelper(root, argv[3], &err)) std::printf("launch failed: %s\n", err.c_str()); }
		if (t % 10 == 9) std::printf("t=%d %s\n", t + 1, host.status().c_str());
	}
	host.stopHelper();
	host.stop();
	dev.stop();
	auto& card = engine.card();
	for (int i = 0; i < 4; ++i)
	{
		std::ofstream f(out + "lex" + std::to_string(i) + ".bin", std::ios::binary);
		for (uint16_t a = 0; a < l960::Lexichip::BusSize; ++a) f.put(char(card.lexichip(i).busRead(a)));
	}
	for (int z = 0; z < 2; ++z)
	{
		std::ofstream d(out + "z80_" + std::to_string(z) + "_dpram.bin", std::ios::binary);
		for (auto b : card.dpram(z).data()) d.put(char(b));
		auto& cpu = card.z80(z);
		for (uint32_t b = 0; b < 4; ++b)
		{
			cpu.setBank(0, b);
			cpu.setBank(1, b);
			std::ofstream lo(out + "z80_" + std::to_string(z) + "_lo" + std::to_string(b) + ".bin", std::ios::binary);
			for (uint32_t a = 0x4000; a < 0x8000; ++a) lo.put(char(cpu.peek(uint16_t(a))));
			std::ofstream hi(out + "z80_" + std::to_string(z) + "_hi" + std::to_string(b) + ".bin", std::ios::binary);
			for (uint32_t a = 0xC000; a < 0x10000; ++a) hi.put(char(cpu.peek(uint16_t(a))));
		}
	}
	std::puts("captured");
	return 0;
}
