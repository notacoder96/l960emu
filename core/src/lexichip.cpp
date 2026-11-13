#include "l960/lexichip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace l960
{
	FILE* g_lexWriteLog = nullptr;  // development hook (tests/lexi_capture): logs every bus write when set

	namespace
	{
		// Internal fixed point: samples are 24-bit signed; accumulators carry 16 extra fraction bits
		constexpr int AccFrac = 16;
		constexpr int64_t Unity = int64_t(1) << AccFrac;

		int32_t sat24(const int64_t _v)
		{
			// ASSUMPTION: stores saturate to 24 bits (classic Lexicon behaviour, unconfirmed for Lexichip III)
			return static_cast<int32_t>(std::clamp<int64_t>(_v, -0x800000, 0x7FFFFF));
		}

		int32_t signExtend(const uint32_t _v, const int _bits)
		{
			const uint32_t m = 1u << (_bits - 1);
			return static_cast<int32_t>((_v ^ m) - m);
		}

		const char* regName(const uint8_t _r, char* _buf, const size_t _n)
		{
			if (_r < 0x20) std::snprintf(_buf, _n, "c%u", _r);
			else if (_r < 0x40) std::snprintf(_buf, _n, "x%u", _r - 0x20u);
			else if (_r < 0x50) std::snprintf(_buf, _n, "%s_out%02u", (_r & 1) ? "LEX" : "TMX", _r - 0x40u);
			else if (_r < 0x60) std::snprintf(_buf, _n, "%s_in%02u", (_r & 1) ? "LEX" : "TMX", _r - 0x40u);
			else if (_r < 0x64) std::snprintf(_buf, _n, "peak%u", _r - 0x60u);
			else if (_r < 0x66) std::snprintf(_buf, _n, "dlmu%u", _r - 0x64u);
			else std::snprintf(_buf, _n, "adf%u", _r);
			return _buf;
		}
	}

	WcsOp WcsOp::decode(const uint8_t _b[5])
	{
		WcsOp o;
		std::copy(_b, _b + 5, o.raw);
		o.imm = _b[0];
		o.unity = _b[1] & 0x01;
		o.neg = (_b[1] >> 1) & 1;
		o.addScale = static_cast<uint8_t>((_b[1] >> 2) & 3);
		if (o.addScale == 0) o.addScale = 4;
		o.addEn = (_b[1] >> 4) & 1;
		o.addAcc = (_b[1] >> 5) & 1;
		o.mulAcc = (_b[1] >> 6) & 1;
		o.dp = (_b[1] >> 7) & 1;
		o.dst = _b[2] & 0x7F;
		o.srcAcc = _b[2] >> 7;
		o.src = _b[3] & 0x7F;
		o.acc = _b[3] >> 7;
		o.inv = _b[4] & 0x01;
		o.dlmu = (_b[4] >> 1) & 1;
		o.noOffset = (_b[4] >> 2) & 1;
		o.noCpc = (_b[4] >> 3) & 1;
		o.mem = (_b[4] >> 4) & 1;
		o.clk0 = (_b[4] >> 5) & 1;
		o.clk1 = (_b[4] >> 6) & 1;
		o.special = (_b[4] >> 7) & 1;
		return o;
	}

	Lexichip::Lexichip() : m_dram(DramWords, 0)
	{
		reset();
	}

	void Lexichip::reset()
	{
		m_bus.fill(0);
		m_regs.fill(0);
		m_acc.fill(0);
		m_peak.fill(0);
		m_prevC.fill(0);
		m_latchPending = 0;
		std::fill(m_dram.begin(), m_dram.end(), 0);
		m_writePtr = 0;
		m_dirty = true;
		m_lines = MaxLines;
	}

	uint8_t Lexichip::busRead(const uint16_t _offset) const
	{
		return _offset < BusSize ? m_bus[_offset] : 0xFF;
	}

	void Lexichip::busWrite(const uint16_t _offset, const uint8_t _value)
	{
		if (_offset >= BusSize) return;
		if (g_lexWriteLog) std::fprintf(g_lexWriteLog, "%p %03X %02X\n", static_cast<const void*>(this), _offset, _value);
		// Wide data latch (seen in the Z80 extension kernel's Lexichip writes): bytes written to $B00-$B04 are held
		// and committed by the next write that completes a word: a WCS plane-0 byte commits the pending $B01-$B04
		// into planes 1-4 of that line, an XMOQ byte-3 write commits the pending $B00-$B02 into bytes 0-2.
		if (_offset >= LatchBase && _offset <= LatchBase + 4) m_latchPending |= 1u << (_offset - LatchBase);
		if (_offset >= XmoqBase && _offset < DlmuBase && (_offset & 3) == 3 && m_latchPending)
		{
			for (uint32_t k = 0; k < 3; ++k)
				if (m_latchPending & (1u << k))
				{
					const uint16_t a = static_cast<uint16_t>(_offset - 3 + k);
					if (m_bus[a] != m_bus[LatchBase + k]) ++m_structureWrites;
					m_bus[a] = m_bus[LatchBase + k];
				}
			m_latchPending = 0;
		}
		if (_offset >= WcsBase && _offset < WcsBase + 0x100 && m_latchPending)
		{
			for (uint32_t p = 1; p <= 4; ++p)
				if (m_latchPending & (1u << p))
				{
					const uint16_t a = static_cast<uint16_t>(WcsBase + 0x100 * p + (_offset - WcsBase));
					if (m_bus[a] != m_bus[LatchBase + p]) ++m_structureWrites;
					m_bus[a] = m_bus[LatchBase + p];
				}
			m_latchPending = 0;
		}
		const bool changed = m_bus[_offset] != _value;
		m_bus[_offset] = _value;
		if (_offset >= WcsBase && _offset < WcsBase + 5 * 0x100) m_dirty = true;
		// program structure (WCS planes b1..b4, XMOQ) as opposed to coefficient (b0) updates
		if (changed && ((_offset >= WcsBase + 0x100 && _offset < WcsBase + 5 * 0x100) || (_offset >= XmoqBase && _offset < DlmuBase)))
			++m_structureWrites;
	}

	void Lexichip::loadUcode(const Ucode& _ucode)
	{
		for (size_t p = 0; p < Ucode::PlaneCount; ++p)
			for (size_t i = 0; i < _ucode.lines; ++i)
				busWrite(static_cast<uint16_t>(WcsBase + 0x100 * p + i), _ucode.wcs[p][i]);
		for (size_t i = 0; i < std::min(_ucode.xmoq.size(), MaxXmoq); ++i)
			for (int k = 0; k < 4; ++k)
				busWrite(static_cast<uint16_t>(XmoqBase + 4 * i + k), static_cast<uint8_t>(_ucode.xmoq[i] >> (8 * k)));
		setProgramLength(_ucode.lines);
	}

	void Lexichip::setSerialIn(const uint32_t _slot, const int32_t _sample)
	{
		if (_slot >= 16 && _slot < 32) m_regs[RegIn0 + (_slot - 16)] = _sample;
	}

	int32_t Lexichip::serialOut(const uint32_t _slot) const
	{
		return _slot < 16 ? m_regs[RegOut0 + _slot] : 0;
	}

	void Lexichip::setMailboxInt(const bool _asserted)
	{
		auto& s = m_bus[StatusOffset];
		s = _asserted ? static_cast<uint8_t>(s & ~0x40) : static_cast<uint8_t>(s | 0x40);  // active low
	}

	void Lexichip::setLineCoefficient(const size_t _line, const double _value)
	{
		if (_line >= MaxLines) return;
		const auto mag = static_cast<int>(std::lround(std::fabs(_value) * 256.0));
		busWrite(static_cast<uint16_t>(WcsBase + _line), static_cast<uint8_t>(std::min(mag, 255)));
		auto b1 = busRead(static_cast<uint16_t>(WcsBase + 0x100 + _line));
		b1 = _value < 0 ? static_cast<uint8_t>(b1 | 2) : static_cast<uint8_t>(b1 & ~2);
		busWrite(static_cast<uint16_t>(WcsBase + 0x100 + _line), b1);
	}

	void Lexichip::decode()
	{
		if (!m_dirty) return;
		for (size_t i = 0; i < MaxLines; ++i)
		{
			const uint8_t b[5] = {m_bus[WcsBase + i], m_bus[WcsBase + 0x100 + i], m_bus[WcsBase + 0x200 + i],
			                      m_bus[WcsBase + 0x300 + i], m_bus[WcsBase + 0x400 + i]};
			m_ops[i] = WcsOp::decode(b);
		}
		m_dirty = false;
	}

	DlmuUnit Lexichip::dlmu(const size_t _unit) const
	{
		auto p = [&](int j) -> uint32_t { return m_bus[DlmuBase + 0x20 * j + _unit]; };
		DlmuUnit d;
		d.step = signExtend(p(0) | (p(1) << 8) | (p(2) << 16), 20);
		d.misc = static_cast<uint8_t>(p(3));
		d.coef = static_cast<uint16_t>(p(4) | (p(5) << 8));
		d.offset = signExtend(p(6) | (p(7) << 8) | (p(8) << 16), 24);
		return d;
	}



	int32_t Lexichip::readReg(const uint8_t _r) const
	{
		// c0..c25 form pairs: the odd register reads the even one as it was at the end of the previous sample
		// (memory reads land in c0/c2/c4 and are consumed as c1/c3/c5; one-pole filters store c14 and read c15)
		if (_r < PairedC && (_r & 1)) return m_prevC[_r >> 1];
		if (_r < RegNone) return m_regs[_r];
		return 0;  // ASSUMPTION: 0x65+ (seen as "adf127" in every program) reads as zero
	}

	void Lexichip::writeReg(const uint8_t _r, const int32_t _v)
	{
		if (_r >= RegNone) return;
		if (_r >= RegPeak0 && _r < RegDlmu0)
		{
			auto& pk = m_peak[_r - RegPeak0];
			pk = std::max(pk, std::abs(_v));
			m_regs[_r] = pk;
			return;
		}
		m_regs[_r] = _v;
	}

	void Lexichip::stepDlmu(const size_t _unit)
	{
		// Coefficient interpolation (LexDis "DLMU Status" page: Coefficient p5:p4, Step p0 (shown x2), Goal p3).
		// The unit moves its coefficient toward the goal when a line flagged ?STEP? (b0 bit 5) uses it.
		auto p = [&](int j) -> uint8_t& { return m_bus[DlmuBase + 0x20 * j + _unit]; };
		int32_t cur = p(4) | (p(5) << 8);
		const int32_t goal = (p(3) << 8) | (p(2) & 0xF0);
		const int32_t step = static_cast<int8_t>(p(0)) * 2;
		if (step > 0 && cur < goal) cur = std::min(cur + step, goal);
		else if (step < 0 && cur > goal) cur = std::max(cur + step, goal);
		else return;
		p(4) = static_cast<uint8_t>(cur);
		p(5) = static_cast<uint8_t>(cur >> 8);
	}

	uint32_t Lexichip::physAddr(const uint32_t _addr) const
	{
		// Linear: the 20-bit XMOQ address (+ DLMU offset) is relative to a pointer that decrements every sample,
		// wrapping over the whole 1M-word DRAM (1M x 16 per Lexichip, service manual parts list). SimpleDelay's
		// taps, spaced 0x18C7 apart, continue across 0x1FFFF -> 0x20000, which rules out 128K regions.
		return (m_writePtr + _addr) & (DramWords - 1);
	}

	void Lexichip::processSample()
	{
		decode();
		uint32_t q = 0;

		for (size_t i = 0; i < m_lines; ++i)
		{
			const WcsOp& o = m_ops[i];
			size_t unit = o.imm & 0x1F;
			if ((o.imm & 0xC0) == 0xC0) unit &= 0x1E;  // as LexDis.dll
			const DlmuUnit d = o.dlmu ? dlmu(unit) : DlmuUnit{};

			// External memory operation (applied at its line)
			if (o.mem)
			{
				if (q < MaxXmoq)
				{
					const auto* e = &m_bus[XmoqBase + 4 * q];
					const auto x = XmoqEntry::decode(e[0] | (e[1] << 8) | (e[2] << 16) | (static_cast<uint32_t>(e[3]) << 24));
					int64_t addr = x.addr;
					if (o.dlmu && !o.noOffset) addr += d.offset;
					const uint32_t phys = physAddr(static_cast<uint32_t>(addr));
					if (x.write) m_dram[phys] = readReg(x.reg);
					else writeReg(x.reg, m_dram[phys]);
				}
				++q;
			}

			// Multiply-accumulate
			int64_t coef;
			if (o.unity) coef = o.neg ? -Unity : Unity;
			else if (o.dlmu)
			{
				// 16-bit DLMU coefficient (x/65536); p6 bit 0 flips the sign (LexDis line formatter)
				int64_t c = o.inv ? (~d.coef & 0xFFFF) : d.coef;
				const bool neg = o.neg != ((m_bus[DlmuBase + 0x20 * 6 + unit] & 1) != 0);
				coef = (c << (AccFrac - 16)) * (neg ? -1 : 1);
			}
			else
			{
				const int c = o.inv ? (~o.imm & 0xFF) : o.imm;
				coef = (int64_t(c) << (AccFrac - 8)) * (o.neg ? -1 : 1);
			}

			const int64_t x = o.mulAcc ? (m_acc[o.srcAcc] >> AccFrac) : readReg(o.src);
			int64_t r = x * coef;
			if (o.addEn)
			{
				const int64_t y = o.addAcc ? m_acc[o.srcAcc] : (int64_t(readReg(o.src)) << AccFrac);
				r += y * o.addScale;
			}
			// The store sees the accumulator as it was before this line's MAC (pipeline): "acc0 = 1.0*x1 ; x0 = acc0"
			// copy idioms and the one-pole feedback idioms only work this way, and the captured programs
			// produce no reverb at all with store-after.
			if (o.dst < RegNone) writeReg(o.dst, sat24(m_acc[o.srcAcc] >> AccFrac));
			m_acc[o.acc] = r;
		}

		for (size_t k = 0; k < PairedC / 2; ++k) m_prevC[k] = m_regs[2 * k];
		// ASSUMPTION: every unit slews once per sample (units without any ?STEP? line, e.g. the block coupling
		// gain d16 on some chips, must still reach their goals)
		for (size_t u = 0; u < DlmuCount; ++u) stepDlmu(u);
		m_memOpsLastSample = q;
		m_writePtr = (m_writePtr - 1) & (DramWords - 1);
	}

	std::string Lexichip::disassemble(const size_t _line)
	{
		decode();
		const auto& o = m_ops[_line];
		char a[32], b[32], c[32], buf[256];
		std::string coef;
		if (o.unity) coef = o.neg ? "-1.0" : "1.0";
		else if (o.dlmu) { std::snprintf(buf, sizeof buf, "d%u", o.imm & 0x1F); coef = buf; }
		else { std::snprintf(buf, sizeof buf, "%s%d/256", o.neg ? "-" : "", o.inv ? (~o.imm & 0xFF) : o.imm); coef = buf; }

		std::string x = o.mulAcc ? "acc" + std::to_string(o.srcAcc) : regName(o.src, a, sizeof a);
		std::snprintf(buf, sizeof buf, "acc%u = %s * %s", o.acc, coef.c_str(), x.c_str());
		std::string s = buf;
		if (o.addEn)
		{
			const std::string y = o.addAcc ? "acc" + std::to_string(o.srcAcc) : regName(o.src, b, sizeof b);
			s += o.addScale == 1 ? " + " + y : " + (" + y + " * " + std::to_string(o.addScale) + ")";
		}
		if (o.dst < RegNone) s += std::string("; ") + regName(o.dst, c, sizeof c) + " = acc" + std::to_string(o.srcAcc);
		if (o.mem) s = "[mem] " + s;
		return s;
	}
}
