// LARC2 bring-up runner: boots boot.rom with larc2.rom in program flash, prints both serial ports,
// and writes the LCD to a PPM. Usage: larc2_run <960L folder> [million instructions] [out.ppm]
#include "l960/larc2/larc2.h"
#include "l960/firmware.h"
#include "testutil.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	fs::path root = firmwareDir(argc, argv);
	if (fs::exists(root / "960L")) root /= "960L";
	const double millions = argc > 2 ? std::atof(argv[2]) : 200.0;
	const std::string ppm = argc > 3 ? argv[3] : "larc2_lcd.ppm";

	std::vector<uint8_t> boot, app;
	L960_CHECK(l960::readFile(root / "Larc2" / "boot.rom", boot));
	L960_CHECK(l960::readFile(root / "Larc2" / "larc2.rom", app));

	l960::larc2::Larc2System sys;
	sys.loadBootRom(boot);
	// The boot ROM saves the application at flash offset 0x180000 (save routine at boot.rom 0x133A4) and boots
	// it from there: "Evan" header, length at +4, compressed flag at +0x10, data at +0x40 (0x11DE0)
	sys.loadFlash(app, 0x180000);

	// Minimal stand-in for the 960L side of the boot handshake (boot.rom 0xAB14 / 0xABB8):
	// LARC2 -> host frames: F0 05 cmd [payload] csum16le F7, csum = 5 + cmd + payload
	// host -> LARC2 frames: F0 05 addr type16le len16le [payload] csum16le F7, csum = 5 + addr + type + len + payload
	// LARC2 cmd 6 = "looking for 960L" -> reply type 9; LARC2 cmd 8 = "ready" -> reply type 7 = boot the application
	std::vector<uint8_t> hostRx;
	auto sendToLarc = [&](uint16_t type) {
		const uint16_t sum = uint16_t(5 + 0 + type + 0);
		const uint8_t f[] = {0xF0, 0x05, 0x00, uint8_t(type), uint8_t(type >> 8), 0x00, 0x00, uint8_t(sum), uint8_t(sum >> 8), 0xF7};
		for (uint8_t b : f) sys.serialInput(1, b);
		std::printf("[host] -> LARC2 message type %u\n", type);
	};
	const bool emulateHost = !std::getenv("L960_NO_HOST");

	std::string line[4];
	sys.onSerialOutput = [&](int port, uint8_t b) {
		if (port == 3)
		{
			if (b == '\n' || b == '\r') { if (!line[3].empty()) std::printf("[debug] %s\n", line[3].c_str()); line[3].clear(); }
			else if (b >= 32 && b < 127) line[3].push_back(char(b));
			else { char h[8]; std::snprintf(h, sizeof h, "<%02x>", b); line[3] += h; }
		}
		else
		{
			if (port == 1 && emulateHost)
			{
				if (b == 0xF0) hostRx.clear();
				hostRx.push_back(b);
				if (b == 0xF7 && hostRx.size() >= 4 && hostRx[1] == 0x05)
				{
					const uint8_t cmd = hostRx[2];
					std::printf("[host] <- LARC2 cmd %u (%zu bytes)\n", cmd, hostRx.size());
					if (cmd == 6) sendToLarc(9);
					else if (cmd == 8) sendToLarc(7);
				}
			}
			char h[8]; std::snprintf(h, sizeof h, "%02x ", b);
			line[port] += h;
			if (line[port].size() > 96) { std::printf("[host-tx%d] %s\n", port, line[port].c_str()); line[port].clear(); }
		}
	};
	int excLog = 0;
	sys.cpu().onException = [&](uint32_t vec, uint32_t pc, uint32_t far, uint32_t fsr) {
		if (excLog++ < 12)
			std::printf("[exc] vector %02X lr=%08X far=%08X fsr=%03X cpsr=%08X r0=%08X r1=%08X r2=%08X r3=%08X r13=%08X\n", vec, pc, far, fsr,
			            sys.cpu().cpsr(), sys.cpu().reg(0), sys.cpu().reg(1), sys.cpu().reg(2), sys.cpu().reg(3), sys.cpu().reg(13));
		if (excLog == 1)
		{
			auto& c = sys.cpu();
			std::printf("  trace:");
			for (uint32_t i = 0; i < 24; ++i) std::printf(" %08X", c.pcTrace[(c.pcTracePos - 24 + i) & 63]);
			std::printf("\n");
		}
	};
	std::map<uint32_t, int> unhandled;
	sys.onUnhandledAccess = [&](uint32_t pa, bool, uint32_t) { ++unhandled[pa & ~0xFFu]; };

	const uint64_t total = static_cast<uint64_t>(millions * 1e6);
	const uint64_t chunk = 10'000'000;
	std::map<uint32_t, uint64_t> pcHist, lrHist;
	for (uint64_t done = 0; done < total; done += chunk)
	{
		if (const char* kp = std::getenv("L960_PRESS"))  // "row,col,atMillions"
		{
			int r = 0, c = 0; double at = 0;
			if (std::sscanf(kp, "%d,%d,%lf", &r, &c, &at) == 3 && done == uint64_t(at * 1e6) / chunk * chunk)
			{
				std::printf("[key] press %d,%d\n", r, c);
				sys.setKey(r, c, true); sys.run(20'000'000); sys.setKey(r, c, false);
			}
		}
		for (uint64_t k = 0; k < chunk; k += 4096)
		{
			sys.run(4096);
			++pcHist[sys.cpu().pc() & ~0x3Fu];
			++lrHist[sys.cpu().reg(14)];
		}
		const auto& s = sys.cpu().stats();
		std::printf("-- %6.0fM instr  pc=%08X cpsr=%08X mmu=%d  irq=%llu swi=%llu dabt=%llu pabt=%llu und=%llu\n",
		            double(done + chunk) / 1e6, sys.cpu().pc(), sys.cpu().cpsr(), sys.cpu().cp15Control() & 1,
		            (unsigned long long)s.irqs, (unsigned long long)s.swi, (unsigned long long)s.dataAborts,
		            (unsigned long long)s.prefetchAborts, (unsigned long long)s.undefined);
	}
	auto top = [](const std::map<uint32_t, uint64_t>& m, const char* name) {
		std::multimap<uint64_t, uint32_t, std::greater<>> t;
		for (auto& [a, n] : m) t.emplace(n, a);
		std::printf("%s:", name);
		int k = 0;
		for (auto& [n, a] : t) { if (k++ == 12) break; std::printf(" %08X(%llu)", a, (unsigned long long)n); }
		std::printf("\n");
	};
	top(pcHist, "hot PC blocks");
	top(lrHist, "hot LR");
	std::printf("GPIO levels %08X, idle slices %llu\n", sys.gpioLevels(), (unsigned long long)sys.idleSlices());
	if (const char* d = std::getenv("L960_DUMP"))
	{
		uint32_t a = uint32_t(std::strtoul(d, nullptr, 16));
		std::printf("dump %08X:", a);
		for (int i = 0; i < 64; ++i) { uint32_t v = 0; sys.cpu().debugRead32(a + 4 * i, v); std::printf(" %08X", v); }
		std::printf("\n");
	}
	for (int p = 1; p < 4; ++p) if (!line[p].empty()) std::printf("[port%d] %s\n", p, line[p].c_str());
	for (auto& [a, n] : unhandled) std::printf("unhandled %08X x%d\n", a, n);

	std::vector<uint8_t> rgb;
	uint32_t w, h;
	const bool on = sys.renderLcd(rgb, w, h);
	std::ofstream f(ppm, std::ios::binary);
	f << "P6\n" << w << " " << h << "\n255\n";
	f.write(reinterpret_cast<const char*>(rgb.data()), std::streamsize(rgb.size()));
	std::printf("LCD %s, %ux%u -> %s\n", on ? "on" : "off", w, h, ppm.c_str());
	return 0;
}
