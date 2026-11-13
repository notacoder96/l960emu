// A running LARC2: the emulated remote plus the host side of its serial link, on its own thread.
//
// Host link framing (from boot.rom 0xAC40 / 0xAEE8):
//   LARC2 -> host: F0 05 cmd [payload] csum16le F7                        csum = 5 + cmd + payload
//   host -> LARC2: F0 05 addr type16le len16le [payload] csum16le F7      csum = 5 + addr + type + len + payload
//   Bytes F0 F7 F9 FB FF inside a frame are sent as FF, (byte ^ 0x80).
// Boot handshake: LARC2 cmd 6 ("looking for 960L") -> host type 9; LARC2 cmd 8 -> host type 7 (boot application).
// After that the application speaks the LICS protocol (LARC2 cmd 10 = connection request), not yet implemented.
#pragma once

#include "l960/larc2/larc2.h"

#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace l960::larc2
{
	struct HostFrame
	{
		uint8_t cmd = 0;
		std::vector<uint8_t> payload;
	};

	// Byte-level codec for the host link
	class HostLinkCodec
	{
	public:
		// Feed a byte sent by the LARC2; returns true when a complete frame was decoded into _out
		bool feed(uint8_t _b, HostFrame& _out);
		static std::vector<uint8_t> encodeToLarc2(uint16_t _type, const std::vector<uint8_t>& _payload, uint8_t _addr = 0);

	private:
		std::vector<uint8_t> m_buf;
		bool m_inFrame = false, m_escape = false;
	};

	class Larc2Device
	{
	public:
		// Key names as printed by the boot ROM's key test (boot.rom 0x1B008), [row][col]
		static const char* const KeyNames[5][8];

		Larc2Device();
		~Larc2Device();

		// _root: the "960L" folder (needs Larc2/boot.rom and Larc2/larc2.rom)
		bool load(const std::filesystem::path& _root, std::string* _error = nullptr);
		void start();
		void stop();
		bool running() const { return m_thread.joinable(); }

		// Thread-safe front panel
		bool pressKey(const std::string& _name, bool _down);
		void setFader(int _i, float _pos01);
		void setFaderTouched(int _i, bool _touched);  // the LARC2 faders are touch-sensitive
		void setJoystick(float _x01, float _y01);

		// Thread-safe LCD snapshot (640x240 RGB); returns a counter that changes when a new frame is ready
		uint64_t lcd(std::vector<uint8_t>& _rgb, uint32_t& _w, uint32_t& _h);

		std::string status() const;
		std::string fpgaTrace();  // debugging (L960_FPGA_TRACE)
		void adOverride(int _slot, int _value) { std::lock_guard<std::mutex> l(m_lock); m_sys.adOverride[size_t(_slot) & 15] = _value; }

		// Host link: once the boot handshake has started the application, LARC2 -> host bytes go to
		// onHostOutput (called on the emulation thread) and hostInput() feeds host -> LARC2 bytes.
		std::function<void(uint8_t)> onHostOutput;
		void hostInput(const uint8_t* _data, size_t _n);
		void setHostOutput(std::function<void(uint8_t)> _f);  // thread-safe way to set onHostOutput
		bool linked() const { return m_linked.load(); }
		uint64_t instructions() const { return m_instr.load(); }

	private:
		void threadFunc();
		void onHostByte(uint8_t _b);

		Larc2System m_sys;
		HostLinkCodec m_codec;
		mutable std::mutex m_lock;  // guards m_sys between the emulation thread and the API
		std::thread m_thread;
		std::atomic<bool> m_quit{false};
		std::atomic<uint64_t> m_instr{0};
		std::vector<uint8_t> m_frame;
		uint32_t m_fw = 640, m_fh = 240;
		uint64_t m_frameNo = 0;
		std::string m_state = "not loaded";
		std::string m_console;
		int m_lastHostCmd = -1;
		std::atomic<bool> m_linked{false};
		std::deque<uint8_t> m_hostRx;  // host -> LARC2 bytes waiting for their slot on the 57600 baud link
		uint64_t m_rxCredit = 0;
	};
}
