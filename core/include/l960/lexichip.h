// Lexichip III model.
//
// Everything here is reconstructed from Lexicon's own LexDis.dll (the Lexichip disassembler shipped on
// the 960L CD), ChamAPI.dll and the algorithm DLLs. Items marked ASSUMPTION are not yet confirmed and are
// the first things to check against hardware captures; each one is isolated in a single place.
//
// Z80-visible register map (offsets from the chip's base, $3000 / $8000 in Z80 space):
//   0x000-0x1FF  unknown (ADF / status readback?)            ASSUMPTION: plain RAM
//   0x200-0x3FF  XMOQ: 128 x 4 bytes {addr[7:0], addr[15:8], addr[23:16], (!write<<7)|reg}
//   0x400-0x51F  DLMU: 32 units x 9 byte-planes, plane j at 0x400 + 0x20*j
//   0x600-0xAFF  WCS:  256 lines x 5 byte-planes, plane k at 0x600 + 0x100*k
//   0xB00-0xBFF  control/status; 0xB18 bit 6 = DPRAM INTL (56301 -> Z80 mailbox) per the service manual
#pragma once

#include "l960/firmware.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace l960
{
	struct WcsOp
	{
		uint8_t raw[5] = {};
		// b0
		uint8_t imm = 0;        // 8-bit coefficient magnitude (x/256), or DLMU select when dlmu=1
		// b1
		bool unity = false;     // coefficient is exactly +-1.0
		bool neg = false;       // coefficient sign
		uint8_t addScale = 1;   // multiplier on the added term: field 1..3, 0 -> 4 (LexDis prints it as an integer)
		bool addEn = false;     // add a second term
		bool addAcc = false;    // second term is an accumulator rather than a register
		bool mulAcc = false;    // multiplicand is an accumulator rather than a register
		bool dp = false;        // "?DP?" (meaning unknown to LexDis itself)
		// b2
		uint8_t dst = 0x7F;     // store destination register; >= 0x65 means no store
		uint8_t srcAcc = 0;     // accumulator used for store / acc operands
		// b3
		uint8_t src = 0x7F;     // multiplicand / addend register
		uint8_t acc = 0;        // accumulator written by the MAC
		// b4
		bool inv = false;       // one's-complement the coefficient
		bool dlmu = false;      // line uses a DLMU (index = imm & 0x1F)
		bool noOffset = false;  // memory address without the DLMU offset
		bool noCpc = false;     // "(No CPC)"
		bool mem = false;       // line performs an external memory op (pops the next XMOQ entry)
		bool clk0 = false, clk1 = false, special = false;

		static WcsOp decode(const uint8_t _b[5]);
	};

	struct XmoqEntry
	{
		uint32_t addr = 0;   // 24-bit offset into delay memory
		uint8_t reg = 0x7F;  // register read into / written from
		bool write = false;

		static XmoqEntry decode(uint32_t _word)
		{
			return {_word & 0xFFFFFF, static_cast<uint8_t>((_word >> 24) & 0x7F), ((_word >> 31) & 1) != 0};
		}
	};

	struct DlmuUnit
	{
		int32_t step = 0;     // bytes 0-2, signed 20-bit   ASSUMPTION: per-sample modulation increment
		uint8_t misc = 0;     // byte 3
		uint16_t coef = 0;    // bytes 4-5, used as the line's coefficient (x/65536)
		int32_t offset = 0;   // bytes 6-8, signed 24-bit address offset
	};

	class Lexichip
	{
	public:
		static constexpr uint16_t BusSize = 0xC00;
		static constexpr uint16_t XmoqBase = 0x200, DlmuBase = 0x400, WcsBase = 0x600, StatusOffset = 0xB18;
		static constexpr uint16_t LatchBase = 0xB00;  // wide data latch, see busWrite()
		static constexpr size_t MaxLines = 256, MaxXmoq = 128, DlmuCount = 32;
		static constexpr uint32_t DramWords = 1u << 20;  // 1M x 16 DRAM per Lexichip (service manual parts list)
		static constexpr uint8_t PairedC = 26;  // c0..c25 are even/odd pairs (see readReg)

		// register operand space (7 bits)
		static constexpr uint8_t RegC0 = 0x00, RegX0 = 0x20, RegOut0 = 0x40, RegIn0 = 0x50, RegPeak0 = 0x60, RegDlmu0 = 0x64;
		static constexpr uint8_t RegNone = 0x65;  // >= : store suppressed / read as 0

		Lexichip();

		void reset();

		// Z80 bus interface
		uint8_t busRead(uint16_t _offset) const;
		void busWrite(uint16_t _offset, uint8_t _value);

		// Same effect as the Z80 bulk loads that ChamAPI::LoadLexichip performs
		void loadUcode(const Ucode& _ucode);

		void setProgramLength(uint16_t _lines) { m_lines = _lines > MaxLines ? MaxLines : _lines; }
		uint16_t programLength() const { return m_lines; }
		uint32_t structureWrites() const { return m_structureWrites; }  // changes when a different program is loaded

		// Serial audio, 24-bit signed samples. Slot n = register RegOut0 + n (0..15) / RegIn0 + (n - 16) (16..31).
		void setSerialIn(uint32_t _slot, int32_t _sample);
		int32_t serialOut(uint32_t _slot) const;

		void setMailboxInt(bool _asserted);  // DPRAM INTL, active low on hardware

		// Runs the program once (one word clock).
		void processSample();

		// Coefficient as written by the host: |c| * 256 rounded into b0, sign into b1 bit 1 (LexDis.dll)
		void setLineCoefficient(size_t _line, double _value);

		const WcsOp& op(size_t _line) { decode(); return m_ops[_line]; }
		std::string disassemble(size_t _line);
		uint32_t memOpsLastSample() const { return m_memOpsLastSample; }
		const std::vector<int32_t>& dram() const { return m_dram; }
		int32_t reg(uint8_t _r) const { return readReg(_r); }  // debugging

	private:
		int32_t readReg(uint8_t _r) const;
		void writeReg(uint8_t _r, int32_t _v);
		void decode();
		DlmuUnit dlmu(size_t _unit) const;
		void stepDlmu(size_t _unit);
		uint32_t physAddr(uint32_t _addr) const;

		std::array<uint8_t, BusSize> m_bus{};
		std::array<WcsOp, MaxLines> m_ops{};
		bool m_dirty = true;
		uint32_t m_structureWrites = 0;
		uint32_t m_latchPending = 0;
		uint16_t m_lines = MaxLines;

		std::array<int32_t, 128> m_regs{};
		std::array<int32_t, PairedC / 2> m_prevC{};  // even c registers at the end of the previous sample
		std::array<int64_t, 2> m_acc{};
		std::array<int32_t, 4> m_peak{};
		std::vector<int32_t> m_dram;
		uint32_t m_writePtr = 0;
		uint32_t m_memOpsLastSample = 0;
	};
}
