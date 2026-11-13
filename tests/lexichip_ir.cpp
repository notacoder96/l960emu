// Dev tool: impulse responses of Lexichip pairs from bus dumps (l960_bridge L960_DUMP_LEX) or DLL algorithms.
// Usage: lexichip_ir dump <prefix> [chipA chipB]   |   lexichip_ir alg <960L dir> <name>
#include "l960/firmware.h"
#include "l960/lexichip.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>


int main(int argc, char** argv)
{
	auto a = std::make_unique<l960::Lexichip>();
	auto b = std::make_unique<l960::Lexichip>();
	const int sr = 48000, N = sr * 6;
	const uint32_t LinkShift = std::getenv("LEX_LINK") ? uint32_t(std::atoi(std::getenv("LEX_LINK"))) : 2;
	if (argc > 2 && std::strcmp(argv[1], "dump") == 0)
	{
		const std::string p = argv[2];
		const int ca = argc > 3 ? std::atoi(argv[3]) : 0, cb = argc > 4 ? std::atoi(argv[4]) : 1;
		for (auto [lx, c] : {std::pair{a.get(), ca}, std::pair{b.get(), cb}})
		{
			std::ifstream f(p + std::to_string(c) + ".bin", std::ios::binary);
			std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
			for (size_t i = 0; i < d.size(); ++i) lx->busWrite(uint16_t(i), uint8_t(d[i]));
		}
	}
	else if (argc > 3)
	{
		l960::Firmware fw;
		std::string err;
		if (!fw.load(argv[2], &err)) { std::printf("%s\n", err.c_str()); return 1; }
		const auto* alg = fw.findAlgorithm(argv[3]);
		if (!alg) { std::printf("no algorithm\n"); return 1; }
		a->loadUcode(alg->singleSpeed[0]);
		if (alg->singleSpeed.size() > 1) b->loadUcode(alg->singleSpeed[1]);
	}
	else { std::printf("usage\n"); return 1; }

	// warm-up so DLMU coefficients reach their goals
	for (int n = 0; n < sr / 2; ++n) { a->processSample(); b->processSample(); }
	const uint32_t inSlots[] = {18, 20, 22};
	std::vector<double> env[2][8], mean[2][8];
	for (auto& c : env) for (auto& e : c) e.assign(N / 4800 + 1, 0.0);
	for (auto& c : mean) for (auto& e : c) e.assign(N / 4800 + 1, 0.0);
	double peak = 0;
	for (int n = 0; n < N; ++n)
	{
		const int32_t imp = n == 0 ? (1 << 22) : 0;
		for (uint32_t s : inSlots) { a->setSerialIn(s, imp); b->setSerialIn(s, imp); }
		a->processSample();
		b->processSample();
		// LEX link: out channel c (odd reg 0x41 + 2c) arrives as partner in channel (c + 2) mod 8 (odd reg 0x51 + 2c')
		for (uint32_t c = 0; c < 8; ++c)
		{
			const uint32_t so = 2 * c + 1, si = 16 + 2 * ((c + LinkShift) & 7) + 1;
			const int32_t ab = a->serialOut(so), ba = b->serialOut(so);
			b->setSerialIn(si, ab);
			a->setSerialIn(si, ba);
		}
		for (int c = 0; c < 2; ++c)
			for (int k = 0; k < 8; ++k)
			{
				const double v = (c ? b : a)->serialOut(2 * k) / 8388608.0;
				env[c][k][n / 4800] += v * v;
				mean[c][k][n / 4800] += v;
				peak = std::max(peak, std::fabs(v));
			}
	}
	std::printf("peak %.3g\n", peak);
	for (int c = 0; c < 2; ++c)
		for (int k = 0; k < 8; ++k)
		{
			double tot = 0;
			for (double e : env[c][k]) tot += e;
			if (tot < 1e-12) continue;
			// AC power per 100 ms window (DC removed: truncation leaves a small constant offset in the tank)
			std::vector<double> db;
			for (size_t w = 0; w + 1 < env[c][k].size(); ++w)
			{
				const double m = mean[c][k][w] / 4800, p = env[c][k][w] / 4800 - m * m;
				db.push_back(p > 1e-30 ? 10 * std::log10(p) : -300.0);
			}
			// RT60 from the slope between the loudest window and the first one 30 dB below it
			size_t pk = 0;
			for (size_t w = 0; w < db.size(); ++w) if (db[w] > db[pk]) pk = w;
			double rt = -1;
			for (size_t w = pk + 1; w < db.size(); ++w)
				if (db[w] < db[pk] - 30) { rt = 0.1 * double(w - pk) * 60.0 / (db[pk] - db[w]); break; }
			std::printf("chip%c TMX_out%02d: energy %.1f dB, RT60 ~%.2f s |", c ? 'B' : 'A', 2 * k, 10 * std::log10(tot / 0.25), rt);
			for (size_t w = 0; w < std::min<size_t>(db.size(), 40); ++w) std::printf(" %.0f", db[w]);
			std::printf("  (dB per 100 ms)\n");
		}
	return 0;
}
