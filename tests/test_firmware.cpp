// Firmware loader: every algorithm's UCODE parses, and every program issues exactly as many external
// memory operations per sample as its XMOQ table has entries (the core check of the WCS bit-4 decode).
#include "l960/firmware.h"
#include "l960/lexichip.h"
#include "testutil.h"

int main(int argc, char** argv)
{
	l960::Firmware fw;
	std::string err;
	L960_CHECK(fw.load(firmwareDir(argc, argv), &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));
	for (const auto& w : fw.warnings) std::printf("warning: %s\n", w.c_str());

	std::printf("ChamDSP.pci: %zu words @ P:$%04X, first word $%06X\n", fw.chamDsp().words.size(), fw.chamDsp().loadAddress, fw.chamDsp().words[0]);
	std::printf("ChamID.pci:  %zu words\n", fw.chamId().words.size());
	std::printf("Z80OS.PCI:   %zu bytes\n", fw.z80Kernel().size());
	L960_CHECK(fw.chamDsp().words.size() == 3864);
	L960_CHECK(fw.chamDsp().words[0] == 0x0C0100);  // JMP $100
	L960_CHECK(fw.z80Kernel().size() == 1024);
	std::printf("%zu algorithm DLLs\n", fw.algorithms().size());
	L960_CHECK(fw.algorithms().size() == 31);

	size_t programs = 0;
	for (const auto& a : fw.algorithms())
	{
		std::printf("%-14s %-22s single:%zu double:%zu\n", a.dllName.c_str(), a.name.c_str(), a.singleSpeed.size(), a.doubleSpeed.size());
		for (const auto* set : {&a.singleSpeed, &a.doubleSpeed})
			for (const auto& u : *set)
			{
				size_t memLines = 0;
				for (size_t i = 0; i < u.lines; ++i) memLines += (u.wcs[4][i] >> 4) & 1;
				L960_CHECK(memLines == u.xmoq.size());
				L960_CHECK(u.xmoq.size() <= l960::Lexichip::MaxXmoq);
				++programs;
			}
	}
	std::printf("%zu Lexichip programs: memory-op lines == XMOQ entries in all of them\n", programs);

	const auto* plate = fw.findAlgorithm("Plate");
	L960_CHECK(plate && plate->singleSpeed.size() == 1);
	L960_CHECK(plate->singleSpeed[0].lines == 256 && plate->singleSpeed[0].xmoq.size() == 98);
	L960_CHECK(plate->doubleSpeed.size() == 2 && plate->doubleSpeed[0].lines == 128);
	std::puts("PASS");
	return 0;
}
