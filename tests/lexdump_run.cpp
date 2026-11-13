// Dev tool: load Lexichip bus dumps (from l960_bridge L960_DUMP_LEX) and probe them with impulses.
#include "l960/engine.h"
#include "l960/lexichip.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <vector>

int main(int argc, char** argv)
{
	const std::string prefix = argc > 1 ? argv[1] : "/tmp/claude-0/lex";
	l960::ReverbCard card;
	for (int i = 0; i < 4; ++i)
	{
		std::ifstream f(prefix + std::to_string(i) + ".bin", std::ios::binary);
		std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
		for (size_t a = 0; a < b.size(); ++a) card.lexichip(i).busWrite(uint16_t(a), uint8_t(b[a]));
		std::printf("chip %d: status $B18=%02X\n", i, card.lexichip(i).busRead(0xB18));
	}
	for (int c = 0; c < 4; ++c)
	{
		auto& lx = card.lexichip(c);
		std::set<int> rin, rout;
		for (size_t i = 0; i < lx.programLength(); ++i)
		{
			const auto& o = lx.op(i);
			if (!o.mulAcc && o.src >= l960::Lexichip::RegIn0 && o.src < l960::Lexichip::RegIn0 + 16) rin.insert(o.src - l960::Lexichip::RegIn0);
			if (o.dst >= l960::Lexichip::RegOut0 && o.dst < l960::Lexichip::RegOut0 + 16) rout.insert(o.dst - l960::Lexichip::RegOut0);
		}
		std::printf("chip %d reads in:", c); for (int v : rin) std::printf(" %d", v);
		std::printf("  writes out:"); for (int v : rout) std::printf(" %d", v);
		std::printf("\n");
	}
	for (int k = 0; k < 8; ++k) std::printf("  %s\n", card.lexichip(0).disassemble(size_t(k)).c_str());
	// impulse into every input slot of chip 0 (and 1), watch all outputs of both
	for (int inChip = 0; inChip < 2; ++inChip)
		for (uint32_t s = 16; s < 32; ++s)
		{
			for (int c = 0; c < 2; ++c) card.lexichip(c).reset();
			for (int i = 0; i < 2; ++i)
			{
				std::ifstream f(prefix + std::to_string(i) + ".bin", std::ios::binary);
				std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
				for (size_t a = 0; a < b.size(); ++a) card.lexichip(i).busWrite(uint16_t(a), uint8_t(b[a]));
			}
			double e[2][16] = {};
			for (int n = 0; n < 24000; ++n)
			{
				card.lexichip(inChip).setSerialIn(s, n == 0 ? (1 << 22) : 0);
				for (int c = 0; c < 2; ++c) card.lexichip(c).processSample();
				for (uint32_t o = 1; o < 16; o += 2)
				{
					const int32_t ab = card.lexichip(0).serialOut(o), ba = card.lexichip(1).serialOut(o);
					card.lexichip(1).setSerialIn(o + 16, ab);
					card.lexichip(0).setSerialIn(o + 16, ba);
				}
				for (int c = 0; c < 2; ++c)
					for (uint32_t o = 0; o < 16; ++o) { const double v = card.lexichip(c).serialOut(o) / 8388608.0; e[c][o] += v * v; }
			}
			bool any = false;
			for (int c = 0; c < 2; ++c) for (int o = 0; o < 16; ++o) if (e[c][o] > 1e-9) any = true;
			if (!any) continue;
			std::printf("in chip%d slot %u ->", inChip, s);
			for (int c = 0; c < 2; ++c) for (int o = 0; o < 16; ++o) if (e[c][o] > 1e-9) std::printf(" c%d.o%d=%.3g", c, o, e[c][o]);
			std::printf("\n");
		}
	return 0;
}
