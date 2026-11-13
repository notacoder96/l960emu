// LARC2: boot ROM + application boot to the point where the application asks the host for a LICS connection.
#include "l960/firmware.h"
#include "l960/larc2/larc2device.h"
#include "testutil.h"

int main(int argc, char** argv)
{
	using namespace l960::larc2;
	std::filesystem::path root = firmwareDir(argc, argv);
	if (std::filesystem::exists(root / "960L")) root /= "960L";

	// codec round trip, including escaped bytes
	{
		const auto f = HostLinkCodec::encodeToLarc2(0x1234, {0x01, 0xF0, 0xFF, 0x7F});
		L960_CHECK(f.front() == 0xF0 && f.back() == 0xF7);
		for (size_t i = 1; i + 1 < f.size(); ++i) L960_CHECK(f[i] != 0xF0 && f[i] != 0xF7);
	}

	std::vector<uint8_t> boot, app;
	L960_CHECK(l960::readFile(root / "Larc2" / "boot.rom", boot));
	L960_CHECK(l960::readFile(root / "Larc2" / "larc2.rom", app));
	Larc2System sys;
	sys.loadBootRom(boot);
	sys.loadFlash(app, 0x180000);

	HostLinkCodec codec;
	bool sawLookingFor960L = false, sawAppConnect = false;
	sys.onSerialOutput = [&](int port, uint8_t b) {
		HostFrame f;
		if (port != 1 || !codec.feed(b, f)) return;
		auto send = [&](uint16_t t) { for (uint8_t x : HostLinkCodec::encodeToLarc2(t, {})) sys.serialInput(1, x); };
		if (f.cmd == 6) { sawLookingFor960L = true; send(9); }
		if (f.cmd == 8) send(7);
		if (f.cmd == 10) sawAppConnect = true;
	};

	for (int i = 0; i < 400 && !sawAppConnect; ++i) sys.run(10'000'000);
	std::printf("instructions %llu, idle slices %llu, data aborts %llu, prefetch aborts %llu\n",
	            (unsigned long long)sys.cpu().instructions(), (unsigned long long)sys.idleSlices(),
	            (unsigned long long)sys.cpu().stats().dataAborts, (unsigned long long)sys.cpu().stats().prefetchAborts);
	L960_CHECK(sawLookingFor960L);
	L960_CHECK(sawAppConnect);

	std::vector<uint8_t> rgb;
	uint32_t w, h;
	L960_CHECK(sys.renderLcd(rgb, w, h));
	L960_CHECK(w == 640 && h == 240);
	size_t lit = 0;
	for (size_t i = 0; i < rgb.size(); i += 3) lit += (rgb[i] | rgb[i + 1] | rgb[i + 2]) != 0;
	std::printf("LCD %ux%u, %zu non-black pixels\n", w, h, lit);
	L960_CHECK(lit > 10000);  // the application's main screen frame is drawn
	std::puts("PASS");
	return 0;
}
