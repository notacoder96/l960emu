#include "l960/larc2/larc2.h"

#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace l960::larc2
{
	namespace
	{
		constexpr double OsTimerHz = 3686400.0;

		// SA-1100 interrupt numbers
		constexpr int IrqGpio11_27 = 11, IrqLcd = 12, IrqUart1 = 15, IrqUart2 = 16, IrqUart3 = 17, IrqOst0 = 26, IrqRtcHz = 30, IrqRtcAlarm = 31;

		inline uint32_t rd32(const std::vector<uint8_t>& _m, uint32_t _o) { uint32_t v; std::memcpy(&v, &_m[_o], 4); return v; }
		inline uint16_t rd16(const std::vector<uint8_t>& _m, uint32_t _o) { uint16_t v; std::memcpy(&v, &_m[_o], 2); return v; }
	}

	// ------------------------------------------------------------------ Flash32

	Flash32::Flash32(const size_t _bytes, const uint32_t _blockBytes) : m_data(_bytes, 0xFF), m_block(_blockBytes)
	{
		m_mode.fill(Mode::Array);
	}

	uint32_t Flash32::read(const uint32_t _off, const int _size)
	{
		uint32_t v = 0;
		for (int i = 0; i < _size; ++i)
		{
			const uint32_t o = (_off + i) % m_data.size();
			const int lane = o & 3;
			uint8_t b;
			switch (m_mode[lane])
			{
			case Mode::Array: b = m_data[o]; break;
			case Mode::Id: b = ((o >> 2) & 1) ? 0xA0 : 0x89; break;  // ASSUMPTION: Intel 28F016-class IDs
			default: b = 0x80; break;                                 // status: ready, no errors
			}
			v |= uint32_t(b) << (8 * i);
		}
		return v;
	}

	void Flash32::write(const uint32_t _off, const uint32_t _v, const int _size)
	{
		for (int i = 0; i < _size; ++i)
		{
			const uint32_t o = (_off + i) % m_data.size();
			const int lane = o & 3;
			const uint8_t c = static_cast<uint8_t>(_v >> (8 * i));
			switch (m_mode[lane])
			{
			case Mode::Program:
				m_data[o] &= c;
				m_dirty = true;
				m_mode[lane] = Mode::Status;
				break;
			case Mode::EraseSetup:
				if (c == 0xD0)
				{
					const uint32_t base = (o / m_block) * m_block;
					for (uint32_t k = lane; k < m_block; k += 4) m_data[base + k] = 0xFF;
					m_dirty = true;
				}
				m_mode[lane] = Mode::Status;
				break;
			default:
				switch (c)
				{
				case 0xFF: m_mode[lane] = Mode::Array; break;
				case 0x90: m_mode[lane] = Mode::Id; break;
				case 0x70: case 0x50: m_mode[lane] = Mode::Status; break;
				case 0x40: case 0x10: m_mode[lane] = Mode::Program; break;
				case 0x20: m_mode[lane] = Mode::EraseSetup; break;
				default: break;
				}
			}
		}
	}

	// ------------------------------------------------------------------ UART (SA-1100 Ser1/Ser3 UART mode)

	uint32_t Uart::sr0() const
	{
		uint32_t v = 0;
		v |= 1;                         // TFS: transmit FIFO always serviceable (instant transmit)
		if (!rx.empty()) v |= 2 | 4;    // RFS + RID (receiver idle with data)  ASSUMPTION: simplified
		return v | sr0Sticky;
	}

	uint32_t Uart::sr1() const
	{
		uint32_t v = 0x4;               // TNF
		if (!rx.empty()) v |= 0x2;      // RNE
		return v;
	}

	bool Uart::interrupt() const
	{
		const uint32_t s0 = sr0();
		const bool rie = cr[3] & 0x08, tie = cr[3] & 0x10, txe = cr[3] & 0x02, rxe = cr[3] & 0x01;
		return (tie && txe && (s0 & 1)) || (rie && rxe && (s0 & 6));
	}

	// ------------------------------------------------------------------ system

	Larc2System::Larc2System() : m_cpu(*this), m_dram(DramBytes, 0), m_boot(BootRomBytes, 0xFF), m_flash(FlashBytes, 128u * 1024u)
	{
		reset();
	}

	void Larc2System::loadBootRom(const std::vector<uint8_t>& _rom)
	{
		std::fill(m_boot.begin(), m_boot.end(), 0xFF);
		std::copy_n(_rom.begin(), std::min<size_t>(_rom.size(), m_boot.size()), m_boot.begin());
	}

	void Larc2System::loadFlash(const std::vector<uint8_t>& _image, const uint32_t _offset)
	{
		auto& d = m_flash.data();
		for (size_t i = 0; i < _image.size() && _offset + i < d.size(); ++i) d[_offset + i] = _image[i];
	}

	void Larc2System::reset()
	{
		m_cpu.reset();
		m_ost = {};
		m_rtc = {};
		m_rtc.rcnr = static_cast<uint32_t>(std::time(nullptr));
		m_ic = {};
		m_gpdr = m_gpioOut = m_grer = m_gfer = m_gedr = m_gafr = 0;
		// ASSUMPTION: idle input levels. GPIO11 = PS/2 TXRDY (input buffer empty) is high after reset.
		// GPIO22 = FPGA_DONE (boot ROM: "FPGA Not Ready!!" unless GPLR bit 22 is set)
		// GPIO4/GPIO7 = PCMCIA card detect, active low (boot ROM 0xC008): socket 0 = on-board flash present,
		// socket 1 = option slot empty
		m_gpioIn = (1u << 7) | (1u << 11) | (1u << 22);
		m_pendingLevel = 0;
		m_rcsr = 1;
		for (auto& u : m_uart) { u.cr.fill(0); u.rx.clear(); u.sr0Sticky = 0; }
		m_lcd = {};
		m_regs.clear();
		m_fpga = {};
		m_fpga.fader.fill(0x200);  // faders mid-travel
		m_fpga.p12v = 0x0333;  // ASSUMPTION: ~12 V through the 1/3 divider on a 5 V, 10-bit A/D
		for (int i = 1; i <= 3; ++i)
			m_uart[i].onTx = [this, i](uint8_t b) { if (onSerialOutput) onSerialOutput(i, b); };
	}

	void Larc2System::serialInput(const int _port, const uint8_t _byte)
	{
		if (_port >= 1 && _port <= 3) m_uart[_port].rx.push_back(_byte);
		updateInterrupts();
	}

	void Larc2System::setKey(const int _row, const int _col, const bool _down)
	{
		if (_row < 0 || _row > 4 || _col < 0 || _col > 7) return;
		if (_down) m_fpga.keys[_row] |= uint8_t(1u << _col); else m_fpga.keys[_row] &= uint8_t(~(1u << _col));
	}

	void Larc2System::setFader(const int _i, const uint16_t _v) { if (_i >= 0 && _i < 8) m_fpga.fader[_i] = _v & 0x3FF; }
	void Larc2System::setFaderTouched(const int _i, const bool _t) { if (_i >= 0 && _i < 8) { if (_t) m_fpga.touch |= uint8_t(1u << _i); else m_fpga.touch &= uint8_t(~(1u << _i)); } }
	void Larc2System::setJoystick(const uint16_t _x, const uint16_t _y) { m_fpga.joyX = _x & 0x3FF; m_fpga.joyY = _y & 0x3FF; }

	void Larc2System::ps2Input(const uint8_t _code)
	{
		m_fpga.ps2.push_back(_code);
		ps2Update();
	}

	// ------------------------------------------------------------------ bus

	const uint32_t* Larc2System::fetchPage(const uint32_t _pa)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) return reinterpret_cast<const uint32_t*>(&m_dram[(_pa - 0xC0000000u) % DramBytes & ~0xFFFu]);
		if (_pa < 0x08000000u) return reinterpret_cast<const uint32_t*>(&m_boot[(_pa % BootRomBytes) & ~0xFFFu]);
		return nullptr;
	}

	bool Larc2System::read32(const uint32_t _pa, uint32_t& _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { _v = rd32(m_dram, (_pa - 0xC0000000u) % DramBytes & ~3u); return true; }
		return readIo(_pa & ~3u, _v, 4);
	}

	bool Larc2System::read16(const uint32_t _pa, uint16_t& _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { _v = rd16(m_dram, (_pa - 0xC0000000u) % DramBytes & ~1u); return true; }
		uint32_t v;
		if (!readIo(_pa & ~1u, v, 2)) return false;
		_v = uint16_t(v);
		return true;
	}

	bool Larc2System::read8(const uint32_t _pa, uint8_t& _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { _v = m_dram[(_pa - 0xC0000000u) % DramBytes]; return true; }
		uint32_t v;
		if (!readIo(_pa, v, 1)) return false;
		_v = uint8_t(v);
		return true;
	}

	bool Larc2System::write32(const uint32_t _pa, const uint32_t _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { std::memcpy(&m_dram[(_pa - 0xC0000000u) % DramBytes & ~3u], &_v, 4); return true; }
		return writeIo(_pa & ~3u, _v, 4);
	}

	bool Larc2System::write16(const uint32_t _pa, const uint16_t _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { std::memcpy(&m_dram[(_pa - 0xC0000000u) % DramBytes & ~1u], &_v, 2); return true; }
		return writeIo(_pa & ~1u, _v, 2);
	}

	bool Larc2System::write8(const uint32_t _pa, const uint8_t _v)
	{
		if (_pa >= 0xC0000000u && _pa < 0xC8000000u) { m_dram[(_pa - 0xC0000000u) % DramBytes] = _v; return true; }
		return writeIo(_pa, _v, 1);
	}

	bool Larc2System::readIo(const uint32_t _pa, uint32_t& _v, const int _size)
	{
		if (_pa < 0x08000000u)  // boot flash, mirrored across static bank 0
		{
			const uint32_t o = _pa % BootRomBytes;
			_v = _size == 4 ? rd32(m_boot, o) : _size == 2 ? rd16(m_boot, o) : m_boot[o];
			return true;
		}
		if (_pa < 0x10000000u) { _v = m_flash.read((_pa - 0x08000000u) % FlashBytes, _size); return true; }
		if (_pa >= 0x3C000000u && _pa < 0x40000000u) { _v = m_flash.read((_pa - 0x3C000000u) % FlashBytes, _size); return true; }  // PCMCIA alias
		if (_pa >= 0x10000000u && _pa < 0x18000000u) { _v = fpgaRead(_pa & 0xFFFF); if (_size == 1) _v = (_v >> (8 * (_pa & 3))) & 0xFF; return true; }
		if (_pa >= 0x18000000u && _pa < 0x20000000u) { _v = m_fpga.cs3; return true; }
		if (_pa >= 0x20000000u && _pa < 0x3C000000u) { _v = 0xFFFFFFFFu; return true; }  // empty PCMCIA space / attribute memory
		if (_pa >= 0x80000000u && _pa < 0xC0000000u)
		{
			const uint32_t w = socRead(_pa & ~3u);
			_v = _size == 4 ? w : (w >> (8 * (_pa & 3))) & (_size == 2 ? 0xFFFF : 0xFF);
			return true;
		}
		if (_pa >= 0xE0000000u && _pa < 0xE8000000u) { _v = 0; return true; }  // zeros bank
		++m_unhandled;
		if (onUnhandledAccess) onUnhandledAccess(_pa, false, 0);
		_v = 0;
		return _pa < 0x40000000u || _pa >= 0x80000000u;  // reserved 0x4..0x7 and 0xE8.. abort
	}

	bool Larc2System::writeIo(const uint32_t _pa, const uint32_t _v, const int _size)
	{
		if (_pa < 0x08000000u) return true;  // boot flash programming not modelled
		if (_pa < 0x10000000u) { m_flash.write((_pa - 0x08000000u) % FlashBytes, _v, _size); return true; }
		if (_pa >= 0x3C000000u && _pa < 0x40000000u) { m_flash.write((_pa - 0x3C000000u) % FlashBytes, _v, _size); return true; }
		if (_pa >= 0x10000000u && _pa < 0x18000000u) { fpgaWrite(_pa & 0xFFFF, _v); return true; }
		if (_pa >= 0x18000000u && _pa < 0x20000000u) { m_fpga.cs3 = _v; return true; }
		if (_pa >= 0x20000000u && _pa < 0x3C000000u) return true;
		if (_pa >= 0x80000000u && _pa < 0xC0000000u)
		{
			uint32_t v = _v;
			if (_size != 4)  // byte/halfword writes to 32-bit registers (UART data registers take bytes)
			{
				const uint32_t sh = 8 * (_pa & 3), m = (_size == 2 ? 0xFFFFu : 0xFFu) << sh;
				v = (socRead(_pa & ~3u) & ~m) | ((_v << sh) & m);
				if ((_pa & 0xFF) == 0x14) v = _v;  // UTDR
			}
			socWrite(_pa & ~3u, v);
			return true;
		}
		if (_pa >= 0xE0000000u && _pa < 0xE8000000u) return true;
		++m_unhandled;
		if (onUnhandledAccess) onUnhandledAccess(_pa, true, _v);
		return _pa < 0x40000000u || _pa >= 0x80000000u;
	}

	// ------------------------------------------------------------------ FPGA

	uint32_t Larc2System::fpgaRead(const uint32_t _off)
	{
		auto& f = m_fpga;
		static const bool trace = std::getenv("L960_FPGA_TRACE") != nullptr;
		if (trace) ++fpgaTrace[_off];
		if (_off < 0x20) return f.motorWave[_off >> 2];
		switch (_off & ~1u)
		{
		case 0x100: return f.motorEnable;
		case 0x102: return f.motorDir;
		case 0x104: return f.motorShift;
		case 0x200: return f.adCmd;
		case 0x400: return f.meter0;
		case 0x402: return f.meter1;
		case 0x404: return f.keyLeds;
		case 0x600: return uint8_t(~f.touch);  // boot ROM self test expects 0xFF with no fader touched
		case 0x700:
		{
			uint8_t b = 0;
			if (!f.ps2.empty()) { b = f.ps2.front(); f.ps2.pop_front(); }
			ps2Update();
			return b;
		}
		case 0x702: case 0x704:
		{
			// bit 0 = output buffer full (byte from keyboard), bit 1 = input buffer full (byte to keyboard)
			return (f.ps2.empty() ? 0u : 1u) | (f.ps2Ibf ? 2u : 0u);
		}
		default: break;
		}
		if (_off >= 0x300 && _off < 0x320)
		{
			const uint32_t idx = (_off - 0x300) >> 1;
			if (adOverride[idx] >= 0) return uint32_t(adOverride[idx]);
			return (f.adValid >> idx) & 1 ? f.adRegs[idx] : adSample(idx >> 3, idx & 7);
		}
		if (_off >= 0x500 && _off < 0x50A) return f.keys[(_off - 0x500) >> 1];  // rows at 0x500, 0x502 .. 0x508 (boot ROM 0x9988)
		const auto it = m_regs.find(0x10000000u | _off);
		return it == m_regs.end() ? 0 : it->second;
	}

	void Larc2System::fpgaWrite(const uint32_t _off, const uint32_t _v)
	{
		static const bool trace = std::getenv("L960_FPGA_TRACE") != nullptr;
		if (trace && _off >= 0x100) ++fpgaTrace[0x80000000u | (_off << 12) | (_v & 0xFFF)];
		auto& f = m_fpga;
		if (_off < 0x20) { f.motorWave[_off >> 2] = _v; return; }
		switch (_off & ~1u)
		{
		case 0x100: f.motorEnable = uint16_t(_v); return;
		case 0x102: f.motorDir = uint16_t(_v); return;
		case 0x104: f.motorShift = uint16_t(_v); return;
		case 0x200:
		{
			// Bit 5 (the CE application writes $20/$60 every pass and then reads all 12 results) or bit 7 selects
			// the continuous scan: results at 0x300.. are live. Otherwise a single conversion of channel
			// bits[3:0] on converter bit 4, latched at 0x300 + 2 * (cmd & 0xF) (boot ROM self-test).
			// A latched single conversion used to stay valid forever, freezing faders 2-8 and the joystick.
			f.adCmd = uint16_t(_v);
			f.adScan = (_v & 0xA0) != 0;
			const uint32_t ch = _v & 0xF;
			if (f.adScan) f.adValid = 0;
			else { f.adRegs[ch] = adSample((_v >> 4) & 1, ch); f.adValid |= uint16_t(1u << ch); }
			return;
		}
		case 0x400: f.meter0 = uint16_t(_v); return;
		case 0x402: f.meter1 = uint16_t(_v); return;
		case 0x404: f.keyLeds = uint16_t(_v); return;
		case 0x700: f.ps2Out = uint8_t(_v); f.ps2Ibf = true; f.ps2Timer = 1e-3; ps2Update(); return;
		default: m_regs[0x10000000u | _off] = _v; return;
		}
	}

	uint16_t Larc2System::adSample(const uint32_t _converter, const uint32_t _channel) const
	{
		// TLV1548 self-test inputs: 0xB = (REF+ - REF-)/2, 0xC = REF-, 0xD = REF+
		if (_channel == 0xB) return 0x200;
		if (_channel == 0xC) return 0x000;
		if (_channel == 0xD) return 0x3FF;
		if (_channel > 7) return 0;
		if (_converter == 0) return m_fpga.fader[_channel];
		switch (_channel)
		{
		// converter 1 channel 0 is the joystick's vertical axis and channel 1 the horizontal one (checked against
		// the 960L's joystick display); higher values are up / right
		case 0: return m_fpga.joyY;
		case 1: return m_fpga.joyX;
		case 2: return m_fpga.p12v;
		case 3: return m_fpga.tap;
		default: return 0;
		}
	}

	void Larc2System::ps2Update()
	{
		// GPIO11 = TXRDY (input buffer empty), GPIO12 = RXRDY (byte from the keyboard waiting)
		const uint32_t old = m_gplr();
		m_gpioIn = (m_gpioIn & ~((1u << 11) | (1u << 12))) | (m_fpga.ps2Ibf ? 0 : (1u << 11)) | (m_fpga.ps2.empty() ? 0 : (1u << 12));
		gpioInputChanged(old, m_gplr());
	}

	// ------------------------------------------------------------------ SA-1100 on-chip registers

	uint32_t Larc2System::socRead(const uint32_t _pa)
	{
		const auto uartIdx = [](uint32_t a) -> int {
			switch (a & 0xFFFF0000u) { case 0x80010000u: return 1; case 0x80030000u: return 2; case 0x80050000u: return 3; default: return 0; }
		};
		if (const int u = uartIdx(_pa))
		{
			auto& s = m_uart[u];
			switch (_pa & 0xFF)
			{
			case 0x00: case 0x04: case 0x08: case 0x0C: return s.cr[(_pa & 0xF) >> 2];
			case 0x14: { uint8_t b = 0; if (!s.rx.empty()) { b = s.rx.front(); s.rx.pop_front(); } updateInterrupts(); return b; }
			case 0x1C: return s.sr0();
			case 0x20: return s.sr1();
			default: break;
			}
		}

		switch (_pa)
		{
		// OS timer
		case 0x90000000u: case 0x90000004u: case 0x90000008u: case 0x9000000Cu: return m_ost.osmr[(_pa & 0xF) >> 2];
		case 0x90000010u:
		{
			// Busy-wait acceleration: firmware delay loops poll OSCR back to back. When two reads are only a
			// few instructions apart, skip 20 us of emulated time instead of executing the loop.
			const uint64_t now = m_cpu.instructions();
			if (now - m_lastOscrRead < 32) advanceTimers(static_cast<uint64_t>(m_cpuHz * 20e-6));
			m_lastOscrRead = now;
			return m_ost.oscr;
		}
		case 0x90000014u: return m_ost.ossr;
		case 0x90000018u: return m_ost.ower;
		case 0x9000001Cu: return m_ost.oier;
		// RTC
		case 0x90010000u: return m_rtc.rtar;
		case 0x90010004u: return m_rtc.rcnr;
		case 0x90010008u: return m_rtc.rttr;
		case 0x90010010u: return m_rtc.rtsr;
		// reset controller
		case 0x90030004u: return m_rcsr;
		// GPIO
		case 0x90040000u: return m_gplr();
		case 0x90040004u: return m_gpdr;
		case 0x90040010u: return m_grer;
		case 0x90040014u: return m_gfer;
		case 0x90040018u: return m_gedr;
		case 0x9004001Cu: return m_gafr;
		// interrupt controller
		case 0x90050000u: { const uint32_t p = (m_pendingLevel | (m_gedr & 0x7FF) | ((m_gedr >> 11) ? (1u << IrqGpio11_27) : 0)); return p & m_ic.icmr & ~m_ic.iclr; }  // ICIP
		case 0x90050004u: return m_ic.icmr;
		case 0x90050008u: return m_ic.iclr;
		case 0x9005000Cu: return m_ic.iccr;
		case 0x90050010u: { const uint32_t p = (m_pendingLevel | (m_gedr & 0x7FF) | ((m_gedr >> 11) ? (1u << IrqGpio11_27) : 0)); return p & m_ic.icmr & m_ic.iclr; }  // ICFP
		case 0x90050020u: return m_pendingLevel | (m_gedr & 0x7FF) | ((m_gedr >> 11) ? (1u << IrqGpio11_27) : 0);  // ICPR
		// LCD
		case 0xB0100004u: return m_lcd.lcsr;
		default: break;
		}
		if (_pa >= 0xB0100000u && _pa < 0xB0100040u) return m_lcd.r[(_pa & 0x3F) >> 2];
		const auto it = m_regs.find(_pa);
		return it == m_regs.end() ? 0 : it->second;
	}

	void Larc2System::socWrite(const uint32_t _pa, const uint32_t _v)
	{
		const auto uartIdx = [](uint32_t a) -> int {
			switch (a & 0xFFFF0000u) { case 0x80010000u: return 1; case 0x80030000u: return 2; case 0x80050000u: return 3; default: return 0; }
		};
		if (const int u = uartIdx(_pa))
		{
			auto& s = m_uart[u];
			switch (_pa & 0xFF)
			{
			case 0x00: case 0x04: case 0x08: case 0x0C: s.cr[(_pa & 0xF) >> 2] = _v; updateInterrupts(); return;
			case 0x14: if (s.onTx) s.onTx(uint8_t(_v)); updateInterrupts(); return;
			case 0x1C: s.sr0Sticky &= ~_v; updateInterrupts(); return;
			default: break;
			}
		}

		switch (_pa)
		{
		case 0x90000000u: case 0x90000004u: case 0x90000008u: case 0x9000000Cu: m_ost.osmr[(_pa & 0xF) >> 2] = _v; return;
		case 0x90000010u: m_ost.oscr = _v; return;
		case 0x90000014u: m_ost.ossr &= ~_v; updateInterrupts(); return;
		case 0x90000018u: m_ost.ower |= _v & 1; return;
		case 0x9000001Cu: m_ost.oier = _v & 0xF; updateInterrupts(); return;
		case 0x90010000u: m_rtc.rtar = _v; return;
		case 0x90010004u: m_rtc.rcnr = _v; return;
		case 0x90010008u: m_rtc.rttr = _v; return;
		case 0x90010010u: m_rtc.rtsr = (m_rtc.rtsr & ~(_v & 3)) | (_v & 0xC); updateInterrupts(); return;
		case 0x90030000u: if (_v & 1) reset(); return;  // RSRR software reset
		case 0x90030004u: m_rcsr &= ~_v; return;
		case 0x90040004u: m_gpdr = _v & 0x0FFFFFFF; return;
		case 0x90040008u: m_gpioOut |= _v; return;
		case 0x9004000Cu: m_gpioOut &= ~_v; return;
		case 0x90040010u: m_grer = _v; return;
		case 0x90040014u: m_gfer = _v; return;
		case 0x90040018u: m_gedr &= ~_v; updateInterrupts(); return;
		case 0x9004001Cu: m_gafr = _v; return;
		case 0x90050004u: m_ic.icmr = _v; updateInterrupts(); return;
		case 0x90050008u: m_ic.iclr = _v; updateInterrupts(); return;
		case 0x9005000Cu: m_ic.iccr = _v; return;
		case 0xB0100004u: m_lcd.lcsr &= ~_v; updateInterrupts(); return;
		case 0xB0100000u:  // LCCR0
		{
			const bool wasOn = m_lcd.r[0] & 1;
			m_lcd.r[0] = _v;
			if (wasOn && !(_v & 1)) m_lcd.lcsr |= 1;  // LDD: disable done
			updateInterrupts();
			return;
		}
		default: break;
		}
		if (_pa >= 0xB0100000u && _pa < 0xB0100040u) { m_lcd.r[(_pa & 0x3F) >> 2] = _v; return; }
		m_regs[_pa] = _v;
	}

	// ------------------------------------------------------------------ timing / interrupts

	void Larc2System::gpioInputChanged(const uint32_t _old, const uint32_t _new)
	{
		const uint32_t rise = ~_old & _new, fall = _old & ~_new;
		m_gedr |= (rise & m_grer) | (fall & m_gfer);
		updateInterrupts();
	}

	void Larc2System::advanceTimers(const uint64_t _instructions)
	{
		const double secs = double(_instructions) / m_cpuHz;

		m_ost.frac += secs * OsTimerHz;
		const auto ticks = static_cast<uint32_t>(m_ost.frac);
		m_ost.frac -= ticks;
		const uint32_t before = m_ost.oscr;
		m_ost.oscr += ticks;
		for (int i = 0; i < 4; ++i)
			if (uint32_t(m_ost.osmr[i] - before - 1) < ticks) m_ost.ossr |= 1u << i;  // match passed in this interval
		if ((m_ost.ower & 1) && (m_ost.ossr & 8)) { reset(); return; }  // watchdog

		if (m_fpga.ps2Ibf && (m_fpga.ps2Timer -= secs) <= 0)
		{
			// the keyboard clocks the byte in and answers (ASSUMPTION: a standard AT keyboard is attached)
			auto& f = m_fpga;
			f.ps2Ibf = false;
			switch (f.ps2Out)
			{
			case 0xFF: f.ps2.push_back(0xFA); f.ps2.push_back(0xAA); break;
			case 0xEE: f.ps2.push_back(0xEE); break;
			case 0xF2: f.ps2.push_back(0xFA); f.ps2.push_back(0xAB); f.ps2.push_back(0x83); break;
			default: f.ps2.push_back(0xFA); break;
			}
			ps2Update();
		}

		m_rtc.frac += secs;
		while (m_rtc.frac >= 1.0)
		{
			m_rtc.frac -= 1.0;
			++m_rtc.rcnr;
			m_rtc.rtsr |= 2;  // HZ
			if (m_rtc.rcnr == m_rtc.rtar) m_rtc.rtsr |= 1;
		}
	}

	void Larc2System::updateInterrupts()
	{
		uint32_t lv = 0;
		lv |= (m_ost.ossr & m_ost.oier) << IrqOst0;
		if (m_uart[1].interrupt()) lv |= 1u << IrqUart1;
		if (m_uart[2].interrupt()) lv |= 1u << IrqUart2;
		if (m_uart[3].interrupt()) lv |= 1u << IrqUart3;
		if ((m_rtc.rtsr & 2) && (m_rtc.rtsr & 8)) lv |= 1u << IrqRtcHz;
		if ((m_rtc.rtsr & 1) && (m_rtc.rtsr & 4)) lv |= 1u << IrqRtcAlarm;
		if ((m_lcd.lcsr & 1) && !(m_lcd.r[0] & (1u << 3))) lv |= 1u << IrqLcd;  // LDD unless LDM masked
		m_pendingLevel = lv;
		const uint32_t pend = lv | (m_gedr & 0x7FF) | ((m_gedr >> 11) ? (1u << IrqGpio11_27) : 0);
		const uint32_t active = pend & m_ic.icmr;
		m_cpu.setIrqLine((active & ~m_ic.iclr) != 0);
		m_cpu.setFiqLine((active & m_ic.iclr) != 0);
	}

	void Larc2System::run(const uint64_t _instructions)
	{
		constexpr uint64_t Slice = 256;
		for (uint64_t done = 0; done < _instructions; done += Slice)
		{
			if (m_cpu.waitingForInterrupt())
			{
				// SA-1100 idle mode: no instructions run until an interrupt; just let time pass
				advanceTimers(Slice);
				updateInterrupts();
				m_cpu.wakeIfInterrupt();
				++m_idleSlices;
				continue;
			}
			m_cpu.run(Slice);
			advanceTimers(Slice);
			updateInterrupts();
		}
	}

	// ------------------------------------------------------------------ LCD

	bool Larc2System::renderLcd(std::vector<uint8_t>& _rgb, uint32_t& _w, uint32_t& _h)
	{
		const auto& r = m_lcd.r;
		const uint32_t lccr1 = r[8], lccr2 = r[9];
		// The LARC2 firmware programs PPL = 639 for its 640-pixel panel, so width is PPL + 1 here
		_w = std::min<uint32_t>((lccr1 & 0x3FF) + 1, 1024);
		_h = std::min<uint32_t>((lccr2 & 0x3FF) + 1, 1024);
		if (!(r[0] & 1) || r[4] < 0xC0000000u) { _rgb.assign(size_t(LcdWidthMax) * LcdHeightMax * 3, 0); _w = LcdWidthMax; _h = LcdHeightMax; return false; }

		const uint32_t base = r[4] - 0xC0000000u;  // DBAR1 (physical)
		const uint16_t pal0 = rd16(m_dram, base % DramBytes);
		const uint32_t bppCode = (pal0 >> 12) & 3;
		const uint32_t bpp = bppCode == 0 ? 4 : bppCode == 1 ? 8 : 16;
		const uint32_t palEntries = bpp == 8 ? 256 : 16;
		const uint32_t pix = base + palEntries * 2;
		const bool color = !(r[0] & 2);  // CMS: 0 = colour

		auto pal = [&](uint32_t i) {
			const uint16_t p = rd16(m_dram, (base + 2 * i) % DramBytes);
			uint8_t rr, gg, bb;
			if (color) { rr = uint8_t(((p >> 8) & 0xF) * 17); gg = uint8_t(((p >> 4) & 0xF) * 17); bb = uint8_t((p & 0xF) * 17); }
			else rr = gg = bb = uint8_t((p & 0xF) * 17);
			return std::array<uint8_t, 3>{rr, gg, bb};
		};

		_rgb.resize(size_t(_w) * _h * 3);
		for (uint32_t y = 0; y < _h; ++y)
			for (uint32_t x = 0; x < _w; ++x)
			{
				std::array<uint8_t, 3> c;
				const uint32_t idx = y * _w + x;
				if (bpp == 16)
				{
					const uint16_t p = rd16(m_dram, (pix + idx * 2) % DramBytes & ~1u);
					c = {uint8_t(((p >> 11) & 31) * 255 / 31), uint8_t(((p >> 5) & 63) * 255 / 63), uint8_t((p & 31) * 255 / 31)};
				}
				else if (bpp == 8) c = pal(m_dram[(pix + idx) % DramBytes]);
				else
				{
					const uint8_t b = m_dram[(pix + idx / 2) % DramBytes];
					c = pal((idx & 1) ? (b >> 4) : (b & 0xF));
				}
				std::copy(c.begin(), c.end(), &_rgb[size_t(idx) * 3]);
			}
		return true;
	}
}
