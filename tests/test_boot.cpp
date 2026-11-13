// Bring-up harness: boots the real card firmware on the emulated processors and reports what it touches.
// This is a diagnostic, not a pass/fail spec: it is how the undocumented protocols get worked out.
#include "l960/card.h"
#include "l960/firmware.h"
#include "testutil.h"

#include <map>

int main(int argc, char** argv)
{
	l960::Firmware fw;
	std::string err;
	L960_CHECK(fw.load(firmwareDir(argc, argv), &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));

	l960::ReverbCard card;
	card.reset();

	// --- Z80 #1: the 56301 would copy Z80OS.PCI into the dual-port RAM, then release Z80_INIT1
	{
		auto& dp = card.dpram(0).data();
		std::copy(fw.z80Kernel().begin(), fw.z80Kernel().end(), dp.begin());
		std::map<uint16_t, int> unmapped;
		card.z80(0).onUnmappedWrite = [&](uint16_t a, uint8_t) { ++unmapped[a]; };
		card.z80(0).setReset(false);

		std::map<uint16_t, int> pcHist;
		for (int i = 0; i < 20000; ++i)
		{
			card.z80(0).run(50);
			++pcHist[card.z80(0).pc()];
		}
		std::printf("Z80 #1: %llu T-states, PC=$%04X, halted=%d, mailbox->56301=%d\n",
		            static_cast<unsigned long long>(card.z80(0).ticks()), card.z80(0).pc(), card.z80(0).halted(), card.dpram(0).intTo56k());
		std::printf("  DPRAM $7FF (vector to 56301) = $%02X\n", dp[0x7FF]);
		std::printf("  hottest PCs (idle loop):");
		std::multimap<int, uint16_t, std::greater<>> hot;
		for (auto& [pc, n] : pcHist) hot.emplace(n, pc);
		int k = 0;
		for (auto& [n, pc] : hot) { if (k++ == 6) break; std::printf(" $%04X(%d)", pc, n); }
		std::printf("\n  unmapped writes: %zu addresses\n", unmapped.size());
		L960_CHECK(card.z80(0).ticks() > 0);
	}

	// --- DSP56301: run ChamDSP.pci and log peripheral / external-bus activity
	if (l960::Dsp56301::available())
	{
		auto& dsp = card.dsp();
		dsp.load(fw.chamDsp());
		dsp.run(200000);
		std::printf("56301: %llu instructions, PC=$%06X, unmapped external accesses %llu\n",
		            static_cast<unsigned long long>(dsp.instructions()), dsp.pc(), static_cast<unsigned long long>(card.unmappedAccesses()));
		std::printf("  peripheral registers touched (X:$FFFFxx):\n");
		for (const auto& [addr, a] : dsp.peripheralLog())
			std::printf("    $%06X  reads %u  writes %u  last $%06X\n", addr, a.reads, a.writes, a.lastValue);
		L960_CHECK(dsp.instructions() >= 200000);
	}
	else
		std::puts("56301 core not built (L960_WITH_DSP56300=OFF)");

	std::puts("DONE");
	return 0;
}
