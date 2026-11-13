// Drives the virtual CHAM driver the way ChamAPI::Init does at start-up, with the real card firmware.
#include "l960/chamdevice.h"
#include "testutil.h"

#include <chrono>

namespace
{
	std::vector<uint8_t> withCard(const std::vector<uint8_t>& _file)
	{
		std::vector<uint8_t> v(4, 0);
		v.insert(v.end(), _file.begin(), _file.end());
		return v;
	}

	std::vector<uint8_t> cmd(std::initializer_list<uint32_t> _w)
	{
		std::vector<uint8_t> v(64, 0);
		size_t i = 0;
		for (uint32_t w : _w) { for (int k = 0; k < 4; ++k) v[4 * i + k] = uint8_t(w >> (8 * k)); ++i; }
		return v;
	}
}

int main(int argc, char** argv)
{
	const auto root = firmwareDir(argc, argv);
	setvbuf(stdout, nullptr, _IONBF, 0);
	l960::ReverbCard card;
	l960::ChamDevice cham(card);
	cham.onLog = [](const std::string& s) { std::printf("  [cham] %s\n", s.c_str()); };
	std::string err;
	L960_CHECK(cham.setFirmwareRoot(root, &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));
	if (!l960::Dsp56301::available()) { std::puts("SKIP: no 56301 core"); return 77; }
	card.dsp().trapAddress = 0x28F;  // ChamDSP's handler for unexpected exceptions (executes "debug")
	int traps = 0;
	for (int i = 0; i < 2; ++i)
		card.dpram(i).onMailbox = [i, &card](char side, uint16_t a, uint8_t v) {
			std::printf("  mailbox Z80#%d %c $%03X <- $%02X (DSP PC $%06X)\n", i, side, a, v, card.dsp().pc());
		};
	card.dsp().onTrap = [&](uint32_t prev, const std::string& regs) {
		if (traps++ < 2) std::printf("TRAP: exception handler reached from PC $%06X\n%s\n", prev, regs.c_str());
	};

	const auto t0 = std::chrono::steady_clock::now();
	auto ms = [&] { return int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()); };

	L960_CHECK(cham.open());
	auto cfg = cham.ioctl(l960::ChamDevice::GetConfiguration, {}, 0x2C);
	L960_CHECK(cfg.info == 0x2C && cfg.out[2] == 1);
	std::printf("open + config: %d ms\n", ms());

	std::vector<uint8_t> dspFile, z80File;
	L960_CHECK(l960::readFile(root / "Cham" / "ChamDSP.pci", dspFile));
	L960_CHECK(l960::readFile(root / "Cham" / "Z80OS.PCI", z80File));
	auto r = cham.ioctl(l960::ChamDevice::Download56KCode, withCard(dspFile), 0);
	std::printf("56K download: info %u ready %d, %d ms, PC $%06X\n", r.info, cham.cardReady(), ms(), card.dsp().pc());
	r = cham.ioctl(l960::ChamDevice::DownloadZ80Kernel, withCard(z80File), 0);
	std::printf("Z80 kernel: info %u ready %d, %d ms, PC $%06X, Z80 PCs $%04X $%04X\n", r.info, cham.cardReady(), ms(), card.dsp().pc(),
	            card.z80(0).pc(), card.z80(1).pc());
	r = cham.ioctl(l960::ChamDevice::InitializeQueues, std::vector<uint8_t>(4, 0), 0);
	std::printf("InitializeQueues: info %u ready %d, %d ms\n", r.info, cham.cardReady(), ms());
	std::printf("  x:$0=$%06X x:$1=$%06X  addrs:", card.dsp().readX(0), card.dsp().readX(1));
	for (uint32_t a = 3; a <= 0x12; ++a) std::printf(" %04X", card.dsp().readX(a));
	std::printf("\n  HTXR words unread: %zu, HRXS words: %zu, master transfers %llu\n", card.dsp().hostTxPending(), card.dsp().hostRxCount(),
	            (unsigned long long)card.dsp().masterTransfers());
	for (const auto& [a, acc] : card.dsp().peripheralLog())
		std::printf("    X:$%06X r%u w%u last $%06X\n", a, acc.reads, acc.writes, acc.lastValue);
	L960_CHECK(r.info == 0 && cham.cardReady());

	// SendTMIXRegisterCommand as ChamAPI builds it: op 1, card 0, unit 4, slot 1, count 3, {tmix+1, reg, value}
	r = cham.ioctl(l960::ChamDevice::TmixCommand, cmd({1, 0, 4, 1, 0, 3, 1, 0x10, 0x55}), 0);
	std::printf("TMIX command: info %u, events %zu (slot %u), %d ms\n", r.info, r.events.size(), r.events.empty() ? 0 : r.events[0], ms());
	L960_CHECK(r.events.size() == 1 && r.events[0] == 1);
	L960_CHECK(card.tmix(0).reg(0x10) == 0x55);

	// Peek56301 X:$500 (host op 5: space 1, addr, mdl 7)
	r = cham.ioctl(l960::ChamDevice::HostCommand, cmd({5, 0, 1, 2, 0, 4, 1, 0x100, 7, 0}), 0);
	std::printf("Peek56301: events %zu, readbacks %zu", r.events.size(), r.readbacks.size());
	if (!r.readbacks.empty()) std::printf(" (mdl %u, %zu bytes)", r.readbacks[0].mdl, r.readbacks[0].bytes.size());
	std::printf("\n");
	L960_CHECK(r.events.size() == 1 && r.readbacks.size() == 1 && r.readbacks[0].mdl == 7);

	// Z80 #0 LoadBulk 6 bytes 11 22 33 44 55 66 to bank 1 $4100, then FetchBulkDump them back (mdl 9)
	r = cham.ioctl(l960::ChamDevice::Z80Command, cmd({1, 0, 2, 2, 0, 10, 6 | 1 << 16, 0x4100, 0x332211, 0x665544}), 0);
	L960_CHECK(r.events.size() == 1);
	std::printf("  DPRAM[0..15]:");
	for (int i = 0; i < 16; ++i) std::printf(" %02x", card.dpram(0).data()[i]);
	std::printf("\n  Z80 peek $4100:");
	for (int i = 0; i < 6; ++i) std::printf(" %02x", card.z80(0).peek(uint16_t(0x4100 + i)));
	std::printf("  $C100:");
	for (int i = 0; i < 6; ++i) std::printf(" %02x", card.z80(0).peek(uint16_t(0xC100 + i)));
	std::printf("\n");
	r = cham.ioctl(l960::ChamDevice::Z80Command, cmd({2, 0, 2, 3, 0, 4, 6 | 1 << 16, 0x4100, 9, 0}), 0);
	std::printf("  DPRAM after fetch:");
	for (int i = 0; i < 16; ++i) std::printf(" %02x", card.dpram(0).data()[i]);
	std::printf("\n  X:$580..:");
	for (int i = 0; i < 8; ++i) std::printf(" %06x", card.dsp().readX(0x580 + i));
	std::printf("\n");
	std::printf("FetchBulkDump: events %zu, readbacks %zu", r.events.size(), r.readbacks.size());
	if (!r.readbacks.empty()) { std::printf(" bytes:"); for (auto b : r.readbacks[0].bytes) std::printf(" %02x", b); }
	std::printf("\n");
	L960_CHECK(r.events.size() == 1 && r.readbacks.size() == 1);
	L960_CHECK((r.readbacks[0].bytes == std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}));

	auto st = cham.ioctl(l960::ChamDevice::GetPeriodicStatus, std::vector<uint8_t>(8, 0), 0);
	st = cham.ioctl(l960::ChamDevice::GetPeriodicStatus, std::vector<uint8_t>(8, 0), 0);
	std::printf("status after 2 requests (PC $%06X):", card.dsp().pc());
	for (size_t i = 0; i < st.out.size(); i += 4) std::printf("%s%06X", i % 32 ? " " : "\n   ", st.out[i] | st.out[i + 1] << 8 | st.out[i + 2] << 16);
	std::printf("\n");
	const auto& s = cham.stats();
	std::printf("commands %llu completions %llu messages %llu timeouts %llu, %d ms\n", (unsigned long long)s.commands,
	            (unsigned long long)s.completions, (unsigned long long)s.messages, (unsigned long long)s.timeouts, ms());
	std::puts("DONE");
	return 0;
}
