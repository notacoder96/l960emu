// Lexichip model: hand-built programs check the MAC/memory semantics; then every factory algorithm runs.
#include "l960/engine.h"
#include "l960/lexichip.h"
#include "testutil.h"

#include <cmath>
#include <vector>

namespace
{
	// Builds a WCS line from fields (inverse of WcsOp::decode)
	void setLine(l960::Lexichip& _lx, size_t _i, uint8_t _b0, uint8_t _b1, uint8_t _b2, uint8_t _b3, uint8_t _b4)
	{
		const uint8_t b[5] = {_b0, _b1, _b2, _b3, _b4};
		for (int p = 0; p < 5; ++p) _lx.busWrite(static_cast<uint16_t>(l960::Lexichip::WcsBase + 0x100 * p + _i), b[p]);
	}

	void setXmoq(l960::Lexichip& _lx, size_t _i, uint32_t _addr, uint8_t _reg, bool _write)
	{
		const uint32_t w = (_addr & 0xFFFFFF) | (uint32_t(_reg & 0x7F) << 24) | (_write ? 0x80000000u : 0);
		for (int k = 0; k < 4; ++k) _lx.busWrite(static_cast<uint16_t>(l960::Lexichip::XmoqBase + 4 * _i + k), static_cast<uint8_t>(w >> (8 * k)));
	}
}

int main(int argc, char** argv)
{
	using L = l960::Lexichip;
	const uint8_t NoStore = 0x7F, In16 = L::RegIn0 + 0, Out0 = L::RegOut0 + 0, X0 = L::RegX0;

	// 1) unity pass-through: acc0 = 1.0 * TMX_in16, then the next line stores it (stores see the
	//    accumulator before their own line's MAC; docs/LEXICHIP.md)
	{
		L lx;
		setLine(lx, 0, 0, 0x01, NoStore, In16, 0x00);
		setLine(lx, 1, 0, 0x00, Out0, NoStore, 0x00);
		lx.setProgramLength(2);
		lx.setSerialIn(16, 123456);
		lx.processSample();
		L960_CHECK(lx.serialOut(0) == 123456);
		std::printf("disasm: %s / %s\n", lx.disassemble(0).c_str(), lx.disassemble(1).c_str());
	}
	// 2) coefficient: 128/256 halves, negative sign flips
	{
		L lx;
		setLine(lx, 0, 128, 0x00, NoStore, In16, 0x00);
		setLine(lx, 1, 0, 0x00, Out0, NoStore, 0x00);
		lx.setProgramLength(2);
		lx.setSerialIn(16, 100000);
		lx.processSample();
		L960_CHECK(lx.serialOut(0) == 50000);
		lx.setLineCoefficient(0, -0.25);
		lx.processSample();
		L960_CHECK(lx.serialOut(0) == -25000);
	}
	// 3) delay line: write input to [0], read it back from [N] -> N-sample delay (pointer decrements)
	{
		L lx;
		const uint32_t N = 10;
		setLine(lx, 0, 0, 0x00, NoStore, NoStore, 0x10);   // mem: [0] = TMX_in16
		setLine(lx, 1, 0, 0x00, NoStore, NoStore, 0x10);   // mem: x0 = [N]
		setLine(lx, 2, 0, 0x01, NoStore, X0, 0x00);        // acc0 = 1.0 * x0
		setLine(lx, 3, 0, 0x00, Out0, NoStore, 0x00);      // TMX_out00 = acc0
		setXmoq(lx, 0, 0, In16, true);
		setXmoq(lx, 1, N, X0, false);
		lx.setProgramLength(4);
		std::vector<int32_t> out;
		for (int n = 0; n < 20; ++n)
		{
			lx.setSerialIn(16, n == 0 ? 1000 : 0);
			lx.processSample();
			out.push_back(lx.serialOut(0));
			L960_CHECK(lx.memOpsLastSample() == 2);
		}
		for (int n = 0; n < 20; ++n) L960_CHECK(out[n] == (n == int(N) ? 1000 : 0));
		std::puts("delay line: impulse appears after exactly N samples");
	}

	// 4) c0..c25 pairs: the odd register reads the even one one sample late
	{
		L lx;
		setLine(lx, 0, 0, 0x01, NoStore, In16, 0x00);      // acc0 = TMX_in16
		setLine(lx, 1, 0, 0x01, 0x00, 0x01, 0x00);         // c0 = acc0 ; acc0 = 1.0 * c1
		setLine(lx, 2, 0, 0x00, Out0, NoStore, 0x00);      // TMX_out00 = acc0
		lx.setProgramLength(3);
		std::vector<int32_t> out;
		for (int n = 0; n < 4; ++n) { lx.setSerialIn(16, n == 0 ? 777 : 0); lx.processSample(); out.push_back(lx.serialOut(0)); }
		L960_CHECK(out[0] == 0 && out[1] == 777 && out[2] == 0);
	}

	// 5) every factory algorithm runs at 48 and 96 kHz without blowing up
	l960::Engine eng;
	std::string err;
	L960_CHECK(eng.loadFirmware(firmwareDir(argc, argv), &err) || (std::fprintf(stderr, "%s\n", err.c_str()), false));
	std::vector<float> inL(4800), inR(4800), o[8];
	for (auto& v : o) v.assign(4800, 0.0f);
	inL[0] = inR[0] = 0.5f;
	const float* ins[2] = {inL.data(), inR.data()};
	float* outs[8];
	for (int c = 0; c < 8; ++c) outs[c] = o[c].data();

	for (const auto& a : eng.firmware()->algorithms())
		for (const double sr : {48000.0, 96000.0})
		{
			if (!eng.selectAlgorithm(a.name, sr, &err)) { std::printf("%-22s %5.0f Hz: %s\n", a.name.c_str(), sr, err.c_str()); continue; }
			eng.process(ins, 2, outs, 8, 4800);
			double energy = 0;
			bool finite = true;
			for (auto& ch : o) for (float v : ch) { energy += double(v) * v; finite &= std::isfinite(v); }
			L960_CHECK(finite);
			size_t nin = 0, nout = 0;
			for (const auto& io : eng.io()) { nin += io.inSlots.size(); nout += io.outSlots.size(); }
			std::printf("%-22s %5.0f Hz: %zu chip(s), %zu in / %zu out slots, mem ops/sample %u, output energy %.3g\n",
			            a.name.c_str(), sr, eng.io().size(), nin, nout, eng.card().lexichip(0).memOpsLastSample(), energy);
		}
	std::puts("PASS");
	return 0;
}
