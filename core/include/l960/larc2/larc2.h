// LARC2 remote: SA-1100 SoC peripherals + the LARC2 board (boot ROM, program flash, FPGA, LCD).
//
// Memory map and board registers from the 960L service manual (LARC2 theory of operation, p. 7-39..7-52).
// SA-1100 peripheral behaviour follows the Intel SA-1100 developer's manual. Items marked ASSUMPTION
// are not yet confirmed against the LARC2 firmware.
#pragma once

#include "l960/larc2/arm4.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace l960::larc2
{
	// Intel-style command user interface, 4 x8 chips side by side on a 32-bit bus (manual: "four 16 MBit
	// Flash memory chips... organized as 2 MBytes by 32 bits"). Commands are written per byte lane.
	class Flash32
	{
	public:
		explicit Flash32(size_t _bytes, uint32_t _blockBytes);
		std::vector<uint8_t>& data() { return m_data; }
		uint32_t read(uint32_t _off, int _size);
		void write(uint32_t _off, uint32_t _v, int _size);
		bool dirty() const { return m_dirty; }

	private:
		enum class Mode : uint8_t { Array, Status, Id, Program, EraseSetup };
		std::vector<uint8_t> m_data;
		uint32_t m_block;
		std::array<Mode, 4> m_mode{};
		bool m_dirty = false;
	};

	struct Uart
	{
		std::array<uint32_t, 4> cr{};  // UTCR0..3
		std::deque<uint8_t> rx;
		std::function<void(uint8_t)> onTx;
		uint32_t sr0Sticky = 0;        // RBB/REB/EIF etc. (write-1-to-clear)
		bool interrupt() const;
		uint32_t sr0() const;
		uint32_t sr1() const;
	};

	class Larc2System final : public ArmBus
	{
	public:
		static constexpr uint32_t DramBytes = 16u << 20;
		static constexpr uint32_t BootRomBytes = 128u << 10;
		static constexpr uint32_t FlashBytes = 8u << 20;
		static constexpr uint32_t LcdWidthMax = 640, LcdHeightMax = 240;

		Larc2System();

		void loadBootRom(const std::vector<uint8_t>& _rom);
		// Program flash contents (e.g. larc2.rom as written by "Update LARC2 Software")
		void loadFlash(const std::vector<uint8_t>& _image, uint32_t _offset = 0);

		void reset();
		// Runs about _instructions CPU instructions, servicing timers and interrupts
		void run(uint64_t _instructions);

		Arm4& cpu() { return m_cpu; }

		// Serial ports: 1 = host link (RS422 to the 960L), 3 = debug console
		void serialInput(int _port, uint8_t _byte);
		std::function<void(int port, uint8_t byte)> onSerialOutput;

		// Front panel inputs
		void setKey(int _row, int _col, bool _down);
		void setFader(int _i, uint16_t _value10bit);
		void setFaderTouched(int _i, bool _touched);
		void setJoystick(uint16_t _x, uint16_t _y);
		void ps2Input(uint8_t _scanCode);

		// LCD: renders the current frame buffer to 24-bit RGB; returns false while the controller is off
		bool renderLcd(std::vector<uint8_t>& _rgb, uint32_t& _w, uint32_t& _h);

		// Debugging: FPGA access histogram (L960_FPGA_TRACE)
		std::map<uint32_t, uint64_t> fpgaTrace;
		std::array<int, 16> adOverride{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
		uint16_t keyLeds() const { return m_fpga.keyLeds; }

		// Debug / bring-up
		std::function<void(uint32_t pa, bool write, uint32_t value)> onUnhandledAccess;
		uint64_t unhandledAccesses() const { return m_unhandled; }
		uint32_t gpioLevels() const { return m_gplr(); }

		// ArmBus
		bool read32(uint32_t _pa, uint32_t& _v) override;
		bool read16(uint32_t _pa, uint16_t& _v) override;
		bool read8(uint32_t _pa, uint8_t& _v) override;
		bool write32(uint32_t _pa, uint32_t _v) override;
		bool write16(uint32_t _pa, uint16_t _v) override;
		bool write8(uint32_t _pa, uint8_t _v) override;
		const uint32_t* fetchPage(uint32_t _pa) override;

	private:
		bool readIo(uint32_t _pa, uint32_t& _v, int _size);
		bool writeIo(uint32_t _pa, uint32_t _v, int _size);
		uint32_t socRead(uint32_t _pa);
		void socWrite(uint32_t _pa, uint32_t _v);
		uint32_t fpgaRead(uint32_t _off);
		void fpgaWrite(uint32_t _off, uint32_t _v);
		void advanceTimers(uint64_t _instructions);
		void updateInterrupts();
		void gpioInputChanged(uint32_t _old, uint32_t _new);
		uint16_t adSample(uint32_t _converter, uint32_t _channel) const;
		void ps2Update();
		uint32_t m_gplr() const { return (m_gpioOut & m_gpdr) | (m_gpioIn & ~m_gpdr); }

		Arm4 m_cpu;
		std::vector<uint8_t> m_dram, m_boot;
		Flash32 m_flash;

		// SA-1100 state
		struct OsTimer { std::array<uint32_t, 4> osmr{}; uint32_t oscr = 0, ossr = 0, ower = 0, oier = 0; double frac = 0; } m_ost;
		struct Rtc { uint32_t rtar = 0, rcnr = 0, rttr = 0, rtsr = 0; double frac = 0; } m_rtc;
		struct Intc { uint32_t icmr = 0, iclr = 0, iccr = 0; } m_ic;
		uint32_t m_gpdr = 0, m_gpioOut = 0, m_gpioIn = 0, m_grer = 0, m_gfer = 0, m_gedr = 0, m_gafr = 0;
		uint32_t m_pendingLevel = 0;  // level sources (peripherals), bit per interrupt number
		uint32_t m_rcsr = 1;          // hardware reset
		std::array<Uart, 4> m_uart;   // index by port number (1, 2, 3 used)
		struct Lcd { std::array<uint32_t, 16> r{}; uint32_t lcsr = 0; } m_lcd;
		std::unordered_map<uint32_t, uint32_t> m_regs;  // plain storage for registers without behaviour

		// LARC2 FPGA (manual "Register Summary")
		struct Fpga
		{
			std::array<uint32_t, 8> motorWave{};
			uint16_t motorEnable = 0, motorDir = 0, motorShift = 0, adCmd = 0;
			std::array<uint16_t, 8> fader{};
			uint16_t joyX = 512, joyY = 512, p12v = 0, tap = 0;
			uint16_t meter0 = 0, meter1 = 0, keyLeds = 0;
			std::array<uint8_t, 5> keys{};
			uint8_t touch = 0;
			std::array<uint16_t, 16> adRegs{};  // results at 0x300 + 2 * (command & 0xF)
			bool adScan = false;
			uint16_t adValid = 0;  // bit per location holding a single-conversion result
			std::deque<uint8_t> ps2;
			uint8_t ps2Out = 0;
			bool ps2Ibf = false;
			double ps2Timer = 0;  // seconds until the keyboard accepts the byte in the input buffer
			uint32_t cs3 = 0;
		} m_fpga;

		uint64_t m_unhandled = 0;
		uint64_t m_lastOscrRead = 0;
		uint64_t m_idleSlices = 0;
	public:
		uint64_t idleSlices() const { return m_idleSlices; }
	private:
		double m_cpuHz = 206.4e6;   // ASSUMPTION: 1 instruction per CPU clock at the SA-1100's 206 MHz
	};
}
