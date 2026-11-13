#include "l960/larc2/arm4.h"

namespace l960::larc2
{
	namespace
	{
		constexpr uint32_t N = 1u << 31, Z = 1u << 30, C = 1u << 29, V = 1u << 28;
		constexpr uint32_t IBit = 1u << 7, FBit = 1u << 6;
		constexpr size_t TlbSize = 1u << 16;

		inline uint32_t ror(uint32_t _v, uint32_t _s) { _s &= 31; return _s ? (_v >> _s) | (_v << (32 - _s)) : _v; }

		int bankIndex(uint32_t _mode)
		{
			switch (_mode & 0x1F)
			{
			case Arm4::Fiq: return 1;
			case Arm4::Irq: return 2;
			case Arm4::Svc: return 3;
			case Arm4::Abt: return 4;
			case Arm4::Und: return 5;
			default: return 0;
			}
		}

		// AP decode -> bit0 privR, bit1 privW, bit2 usrR, bit3 usrW (SA-1100: S = ctrl bit 8, R = bit 9)
		uint8_t apPerm(uint32_t _ap, bool _s, bool _r)
		{
			switch (_ap & 3)
			{
			case 0:
				if (_s && !_r) return 0x1;
				if (!_s && _r) return 0x5;
				return 0;
			case 1: return 0x3;
			case 2: return 0x7;
			default: return 0xF;
			}
		}
	}

	Arm4::Arm4(ArmBus& _bus) : m_bus(_bus), m_tlb(TlbSize)
	{
		reset();
	}

	void Arm4::reset()
	{
		m_r.fill(0);
		for (auto& b : m_bank) b.fill(0);
		m_spsr.fill(0);
		m_cpsr = Svc | IBit | FBit;
		m_ctrl = 0; m_ttb = 0; m_dacr = 0; m_fsr = 0; m_far = 0; m_pid = 0;
		flushTlb();
		m_instr = 0;
		m_stats = {};
	}

	// ------------------------------------------------------------------ modes

	void Arm4::switchBank(const uint32_t _oldMode, const uint32_t _newMode)
	{
		const int o = bankIndex(_oldMode), n = bankIndex(_newMode);
		if (o == n) return;
		if (o == 1) { for (int i = 0; i < 7; ++i) m_bank[1][i] = m_r[8 + i]; }
		else { for (int i = 0; i < 5; ++i) m_bank[0][i] = m_r[8 + i]; m_bank[o][5] = m_r[13]; m_bank[o][6] = m_r[14]; }
		if (n == 1) { for (int i = 0; i < 7; ++i) m_r[8 + i] = m_bank[1][i]; }
		else { for (int i = 0; i < 5; ++i) m_r[8 + i] = m_bank[0][i]; m_r[13] = m_bank[n][5]; m_r[14] = m_bank[n][6]; }
	}

	void Arm4::setCpsr(const uint32_t _v)
	{
		const uint32_t oldMode = m_cpsr & 0x1F, newMode = _v & 0x1F;
		if (oldMode != newMode) switchBank(oldMode, newMode);
		m_cpsr = _v;
	}

	uint32_t* Arm4::spsrPtr()
	{
		const int b = bankIndex(m_cpsr);
		return b == 0 ? nullptr : &m_spsr[b];
	}

	void Arm4::exception(const uint32_t _vectorOffset, const Mode _mode, const uint32_t _lr, const bool _disableFiq)
	{
		if (onException && _vectorOffset != 0x18 && _vectorOffset != 0x08) onException(_vectorOffset, _lr, m_far, m_fsr);
		const uint32_t old = m_cpsr;
		setCpsr((old & ~0x3Fu) | _mode | IBit | (_disableFiq ? FBit : 0));
		m_spsr[bankIndex(_mode)] = old;
		m_r[14] = _lr;
		m_r[15] = ((m_ctrl & (1u << 13)) ? 0xFFFF0000u : 0u) + _vectorOffset;
		m_branched = true;
	}

	// ------------------------------------------------------------------ MMU

	void Arm4::fault(const uint32_t _status, const uint32_t _domain, const uint32_t _mva, const Access _acc)
	{
		if (_acc == Access::Fetch) return;  // ARMv4: prefetch aborts do not update FSR/FAR
		m_fsr = (_status & 0xF) | ((_domain & 0xF) << 4);
		m_far = _mva;
	}

	bool Arm4::walk(const uint32_t _mva, const Access _acc, const bool _priv, uint32_t& _pa, TlbEntry* _fill)
	{
		uint32_t l1;
		if (!m_bus.read32((m_ttb & 0xFFFFC000u) | ((_mva >> 20) << 2), l1)) { fault(0xC, 0, _mva, _acc); return false; }
		const uint32_t domain = (l1 >> 5) & 0xF;
		const bool s = m_ctrl & (1u << 8), r = m_ctrl & (1u << 9);
		uint32_t ap = 0, pa = 0, pageBase = 0;
		bool section = false, cacheable = false;

		switch (l1 & 3)
		{
		case 0: fault(0x5, 0, _mva, _acc); return false;
		case 2:
			section = true;
			ap = (l1 >> 10) & 3;
			pa = (l1 & 0xFFF00000u) | (_mva & 0xFFFFF);
			pageBase = pa & ~0xFFFu;
			cacheable = true;
			break;
		default:
		{
			const uint32_t l2a = (l1 & 3) == 1 ? (l1 & 0xFFFFFC00u) | (((_mva >> 12) & 0xFF) << 2)
			                                   : (l1 & 0xFFFFF000u) | (((_mva >> 10) & 0x3FF) << 2);
			uint32_t l2;
			if (!m_bus.read32(l2a, l2)) { fault(0xE, domain, _mva, _acc); return false; }
			switch (l2 & 3)
			{
			case 0: fault(0x7, domain, _mva, _acc); return false;
			case 1:  // large 64K
			{
				ap = (l2 >> (4 + 2 * ((_mva >> 14) & 3))) & 3;
				pa = (l2 & 0xFFFF0000u) | (_mva & 0xFFFF);
				const uint32_t aps = (l2 >> 4) & 0xFF;
				cacheable = aps == 0x00 || aps == 0x55 || aps == 0xAA || aps == 0xFF;
				break;
			}
			case 2:  // small 4K
			{
				ap = (l2 >> (4 + 2 * ((_mva >> 10) & 3))) & 3;
				pa = (l2 & 0xFFFFF000u) | (_mva & 0xFFF);
				const uint32_t aps = (l2 >> 4) & 0xFF;
				cacheable = aps == 0x00 || aps == 0x55 || aps == 0xAA || aps == 0xFF;
				break;
			}
			default:  // tiny 1K (fine tables only)
				if ((l1 & 3) != 3) { fault(0x7, domain, _mva, _acc); return false; }
				ap = (l2 >> 4) & 3;
				pa = (l2 & 0xFFFFFC00u) | (_mva & 0x3FF);
				break;
			}
			pageBase = pa & ~0xFFFu;
		}
		}

		const uint32_t dom = (m_dacr >> (2 * domain)) & 3;
		if (dom == 0 || dom == 2) { fault(section ? 0x9 : 0xB, domain, _mva, _acc); return false; }
		const uint8_t perm = dom == 3 ? 0xF : apPerm(ap, s, r);

		const bool write = _acc == Access::Write;
		const uint8_t need = static_cast<uint8_t>(_priv ? (write ? 0x2 : 0x1) : (write ? 0x8 : 0x4));
		if (!(perm & need)) { fault(section ? 0xD : 0xF, domain, _mva, _acc); return false; }

		if (_fill && cacheable)
		{
			_fill->tag = _mva >> 12;
			_fill->paPage = pageBase;
			_fill->perm = perm;
			_fill->gen = m_tlbGen;
		}
		_pa = pa;
		return true;
	}

	bool Arm4::translate(const uint32_t _va, const Access _acc, const bool _priv, uint32_t& _pa)
	{
		if (!(m_ctrl & 1)) { _pa = _va; return true; }
		const uint32_t m = mva(_va);
		TlbEntry& e = m_tlb[(m >> 12) & (TlbSize - 1)];
		if (e.gen == m_tlbGen && e.tag == (m >> 12))
		{
			const bool write = _acc == Access::Write;
			const uint8_t need = static_cast<uint8_t>(_priv ? (write ? 0x2 : 0x1) : (write ? 0x8 : 0x4));
			if (e.perm & need) { _pa = e.paPage | (m & 0xFFF); return true; }
		}
		return walk(m, _acc, _priv, _pa, &e);
	}

	bool Arm4::load32(const uint32_t _va, uint32_t& _v, const bool _priv)
	{
		if ((m_ctrl & 2) && (_va & 3)) { fault(0x1, 0, mva(_va), Access::Read); m_aborted = true; return false; }
		uint32_t pa;
		if (!translate(_va & ~3u, Access::Read, _priv, pa)) { m_aborted = true; return false; }
		uint32_t w;
		if (!m_bus.read32(pa, w)) { fault(0x8, 0, mva(_va), Access::Read); m_aborted = true; return false; }
		_v = ror(w, (_va & 3) * 8);
		return true;
	}

	bool Arm4::load16(const uint32_t _va, uint16_t& _v, const bool _priv)
	{
		if ((m_ctrl & 2) && (_va & 1)) { fault(0x1, 0, mva(_va), Access::Read); m_aborted = true; return false; }
		uint32_t pa;
		if (!translate(_va & ~1u, Access::Read, _priv, pa)) { m_aborted = true; return false; }
		if (!m_bus.read16(pa, _v)) { fault(0x8, 0, mva(_va), Access::Read); m_aborted = true; return false; }
		return true;
	}

	bool Arm4::load8(const uint32_t _va, uint8_t& _v, const bool _priv)
	{
		uint32_t pa;
		if (!translate(_va, Access::Read, _priv, pa)) { m_aborted = true; return false; }
		if (!m_bus.read8(pa, _v)) { fault(0x8, 0, mva(_va), Access::Read); m_aborted = true; return false; }
		return true;
	}

	bool Arm4::store32(const uint32_t _va, const uint32_t _v, const bool _priv)
	{
		if ((m_ctrl & 2) && (_va & 3)) { fault(0x1, 0, mva(_va), Access::Write); m_aborted = true; return false; }
		uint32_t pa;
		if (!translate(_va & ~3u, Access::Write, _priv, pa)) { m_aborted = true; return false; }
		if (!m_bus.write32(pa, _v)) { fault(0x8, 0, mva(_va), Access::Write); m_aborted = true; return false; }
		return true;
	}

	bool Arm4::store16(const uint32_t _va, const uint16_t _v, const bool _priv)
	{
		if ((m_ctrl & 2) && (_va & 1)) { fault(0x1, 0, mva(_va), Access::Write); m_aborted = true; return false; }
		uint32_t pa;
		if (!translate(_va & ~1u, Access::Write, _priv, pa)) { m_aborted = true; return false; }
		if (!m_bus.write16(pa, _v)) { fault(0x8, 0, mva(_va), Access::Write); m_aborted = true; return false; }
		return true;
	}

	bool Arm4::store8(const uint32_t _va, const uint8_t _v, const bool _priv)
	{
		uint32_t pa;
		if (!translate(_va, Access::Write, _priv, pa)) { m_aborted = true; return false; }
		if (!m_bus.write8(pa, _v)) { fault(0x8, 0, mva(_va), Access::Write); m_aborted = true; return false; }
		return true;
	}

	bool Arm4::fetch(const uint32_t _va, uint32_t& _op)
	{
		if (m_flatFetch > 0)
		{
			--m_flatFetch;
			m_fetchPageVa = 1;
			return m_bus.read32(_va & ~3u, _op);
		}
		const bool priv = privileged();
		if ((_va & ~0xFFFu) == m_fetchPageVa && priv == m_fetchPriv)
		{
			if (m_fetchHost) { _op = m_fetchHost[(_va & 0xFFF) >> 2]; return true; }
			return m_bus.read32(m_fetchPa | (_va & 0xFFF), _op);
		}
		uint32_t pa;
		if (!translate(_va, Access::Fetch, priv, pa)) return false;
		m_fetchPageVa = _va & ~0xFFFu;
		m_fetchPriv = priv;
		m_fetchPa = pa & ~0xFFFu;
		m_fetchHost = m_bus.fetchPage(m_fetchPa);
		if (m_fetchHost) { _op = m_fetchHost[(_va & 0xFFF) >> 2]; return true; }
		return m_bus.read32(pa & ~3u, _op);
	}

	bool Arm4::debugRead32(const uint32_t _va, uint32_t& _v)
	{
		uint32_t pa;
		const uint32_t fsr = m_fsr, far = m_far;
		const bool ok = translate(_va, Access::Read, true, pa) && m_bus.read32(pa & ~3u, _v);
		m_fsr = fsr; m_far = far;
		return ok;
	}

	// ------------------------------------------------------------------ execution

	bool Arm4::cond(const uint32_t _c) const
	{
		const bool n = m_cpsr & N, z = m_cpsr & Z, c = m_cpsr & C, v = m_cpsr & V;
		switch (_c)
		{
		case 0x0: return z;
		case 0x1: return !z;
		case 0x2: return c;
		case 0x3: return !c;
		case 0x4: return n;
		case 0x5: return !n;
		case 0x6: return v;
		case 0x7: return !v;
		case 0x8: return c && !z;
		case 0x9: return !c || z;
		case 0xA: return n == v;
		case 0xB: return n != v;
		case 0xC: return !z && n == v;
		case 0xD: return z || n != v;
		case 0xE: return true;
		default: return false;  // NV: never on ARMv4
		}
	}

	uint64_t Arm4::run(const uint64_t _instructions)
	{
		uint64_t i = 0;
		for (; i < _instructions; ++i) step();
		return i;
	}

	void Arm4::undefined()
	{
		++m_stats.undefined;
		exception(0x04, Und, m_r[15] - 4, false);  // m_r[15] = current + 8 during execution
	}

	void Arm4::step()
	{
		++m_instr;
		uint32_t pc = m_r[15];

		if (m_fiqLine && !(m_cpsr & FBit)) { ++m_stats.fiqs; exception(0x1C, Fiq, pc + 4, true); pc = m_r[15]; }
		else if (m_irqLine && !(m_cpsr & IBit)) { ++m_stats.irqs; exception(0x18, Irq, pc + 4, false); pc = m_r[15]; }

		pcTrace[pcTracePos++ & 63] = pc;
		uint32_t op;
		if (!fetch(pc, op)) { ++m_stats.prefetchAborts; exception(0x0C, Abt, pc + 4, false); return; }

		m_r[15] = pc + 8;
		m_aborted = false;
		m_branched = false;
		const uint32_t next = pc + 4;

		if (cond(op >> 28))
		{
			switch ((op >> 25) & 7)
			{
			case 0:
				if ((op & 0x0FFFFFF0) == 0x012FFF10) { setPc(m_r[op & 0xF] & ~3u); break; }  // BX (no Thumb)
				if ((op & 0x0FBF0FFF) == 0x010F0000) { execMrs(op); break; }
				if ((op & 0x0FB0FFF0) == 0x0120F000) { execMsr(op); break; }
				if ((op & 0x0FC000F0) == 0x00000090) { execMultiply(op); break; }
				if ((op & 0x0F8000F0) == 0x00800090) { execMultiplyLong(op); break; }
				if ((op & 0x0FB00FF0) == 0x01000090) { execSwap(op); break; }
				if ((op & 0x0E000090) == 0x00000090) { execHalfword(op); break; }
				if ((op & 0x01900000) == 0x01000000) { undefined(); return; }  // TST..CMN with S=0, not MRS/MSR
				execDataProc(op);
				break;
			case 1:
				if ((op & 0x0FB0F000) == 0x0320F000) { execMsr(op); break; }
				if ((op & 0x01900000) == 0x01000000) { undefined(); return; }
				execDataProc(op);
				break;
			case 2:
				execSingle(op);
				break;
			case 3:
				if (op & 0x10) { undefined(); return; }
				execSingle(op);
				break;
			case 4:
				execBlock(op);
				break;
			case 5:
			{
				const int32_t off = static_cast<int32_t>(op << 8) >> 6;
				if (op & (1u << 24)) m_r[14] = next;
				setPc(static_cast<uint32_t>(static_cast<int32_t>(pc + 8) + off));
				break;
			}
			case 6:
				undefined();
				return;
			case 7:
				if (op & (1u << 24)) { ++m_stats.swi; exception(0x08, Svc, next, false); return; }
				if (op & 0x10) { execCoproc(op); break; }
				undefined();
				return;
			}
		}

		if (m_aborted) { ++m_stats.dataAborts; exception(0x10, Abt, pc + 8, false); return; }
		if (!m_branched) m_r[15] = next;
	}

	uint32_t Arm4::shifterOperand(const uint32_t _op, bool& _carry)
	{
		_carry = m_cpsr & C;
		if (_op & (1u << 25))
		{
			const uint32_t rot = ((_op >> 8) & 0xF) * 2;
			const uint32_t v = ror(_op & 0xFF, rot);
			if (rot) _carry = v >> 31;
			return v;
		}
		const uint32_t rm = m_r[_op & 0xF] + ((_op & 0x10) && (_op & 0xF) == 15 ? 4 : 0);
		const uint32_t type = (_op >> 5) & 3;
		uint32_t amt;
		if (_op & 0x10)
		{
			amt = m_r[(_op >> 8) & 0xF] & 0xFF;
			if (amt == 0) return rm;
			switch (type)
			{
			case 0:
				if (amt < 32) { _carry = (rm >> (32 - amt)) & 1; return rm << amt; }
				_carry = amt == 32 ? (rm & 1) : 0;
				return 0;
			case 1:
				if (amt < 32) { _carry = (rm >> (amt - 1)) & 1; return rm >> amt; }
				_carry = amt == 32 ? (rm >> 31) : 0;
				return 0;
			case 2:
				if (amt < 32) { _carry = (static_cast<int32_t>(rm) >> (amt - 1)) & 1; return static_cast<uint32_t>(static_cast<int32_t>(rm) >> amt); }
				_carry = rm >> 31;
				return (rm >> 31) ? 0xFFFFFFFFu : 0;
			default:
				amt &= 31;
				if (amt == 0) { _carry = rm >> 31; return rm; }
				_carry = (rm >> (amt - 1)) & 1;
				return ror(rm, amt);
			}
		}
		amt = (_op >> 7) & 31;
		switch (type)
		{
		case 0:
			if (amt == 0) return rm;
			_carry = (rm >> (32 - amt)) & 1;
			return rm << amt;
		case 1:
			if (amt == 0) { _carry = rm >> 31; return 0; }
			_carry = (rm >> (amt - 1)) & 1;
			return rm >> amt;
		case 2:
			if (amt == 0) { _carry = rm >> 31; return (rm >> 31) ? 0xFFFFFFFFu : 0; }
			_carry = (static_cast<int32_t>(rm) >> (amt - 1)) & 1;
			return static_cast<uint32_t>(static_cast<int32_t>(rm) >> amt);
		default:
			if (amt == 0)
			{
				const uint32_t c = (m_cpsr & C) ? 1u : 0u;
				_carry = rm & 1;
				return (rm >> 1) | (c << 31);
			}
			_carry = (rm >> (amt - 1)) & 1;
			return ror(rm, amt);
		}
	}

	void Arm4::execDataProc(const uint32_t _op)
	{
		bool sc;
		const uint32_t b = shifterOperand(_op, sc);
		const uint32_t rnIdx = (_op >> 16) & 0xF, rd = (_op >> 12) & 0xF;
		const uint32_t a = m_r[rnIdx] + ((_op & 0x02000010) == 0x10 && rnIdx == 15 ? 4 : 0);
		const bool s = _op & (1u << 20);
		const uint32_t opc = (_op >> 21) & 0xF;
		uint64_t r64 = 0;
		uint32_t r = 0;
		bool carry = sc, ovf = (m_cpsr & V) != 0, logical = false, write = true;
		const uint32_t cin = (m_cpsr & C) ? 1 : 0;

		switch (opc)
		{
		case 0x0: r = a & b; logical = true; break;
		case 0x1: r = a ^ b; logical = true; break;
		case 0x2: r = a - b; carry = a >= b; ovf = ((a ^ b) & (a ^ r)) >> 31; break;
		case 0x3: r = b - a; carry = b >= a; ovf = ((b ^ a) & (b ^ r)) >> 31; break;
		case 0x4: r64 = uint64_t(a) + b; r = uint32_t(r64); carry = r64 >> 32; ovf = (~(a ^ b) & (a ^ r)) >> 31; break;
		case 0x5: r64 = uint64_t(a) + b + cin; r = uint32_t(r64); carry = r64 >> 32; ovf = (~(a ^ b) & (a ^ r)) >> 31; break;
		case 0x6: r64 = uint64_t(a) - b - (1 - cin); r = uint32_t(r64); carry = uint64_t(a) >= uint64_t(b) + (1 - cin); ovf = ((a ^ b) & (a ^ r)) >> 31; break;
		case 0x7: r64 = uint64_t(b) - a - (1 - cin); r = uint32_t(r64); carry = uint64_t(b) >= uint64_t(a) + (1 - cin); ovf = ((b ^ a) & (b ^ r)) >> 31; break;
		case 0x8: r = a & b; logical = true; write = false; break;
		case 0x9: r = a ^ b; logical = true; write = false; break;
		case 0xA: r = a - b; carry = a >= b; ovf = ((a ^ b) & (a ^ r)) >> 31; write = false; break;
		case 0xB: r64 = uint64_t(a) + b; r = uint32_t(r64); carry = r64 >> 32; ovf = (~(a ^ b) & (a ^ r)) >> 31; write = false; break;
		case 0xC: r = a | b; logical = true; break;
		case 0xD: r = b; logical = true; break;
		case 0xE: r = a & ~b; logical = true; break;
		default: r = ~b; logical = true; break;
		}
		(void)logical;

		if (write)
		{
			if (rd == 15)
			{
				if (s) { if (auto* sp = spsrPtr()) setCpsr(*sp); }
				setPc(r);
				return;
			}
			m_r[rd] = r;
		}
		if (s)
		{
			uint32_t f = m_cpsr & ~(N | Z | C | V);
			if (r & 0x80000000u) f |= N;
			if (r == 0) f |= Z;
			if (carry) f |= C;
			if (ovf) f |= V;
			m_cpsr = f;
		}
	}

	void Arm4::execMultiply(const uint32_t _op)
	{
		const uint32_t rd = (_op >> 16) & 0xF, rn = (_op >> 12) & 0xF, rs = (_op >> 8) & 0xF, rm = _op & 0xF;
		uint32_t r = m_r[rm] * m_r[rs];
		if (_op & (1u << 21)) r += m_r[rn];
		m_r[rd] = r;
		if (_op & (1u << 20)) m_cpsr = (m_cpsr & ~(N | Z)) | (r & N) | (r ? 0 : Z);
	}

	void Arm4::execMultiplyLong(const uint32_t _op)
	{
		const uint32_t hi = (_op >> 16) & 0xF, lo = (_op >> 12) & 0xF, rs = (_op >> 8) & 0xF, rm = _op & 0xF;
		uint64_t r;
		if (_op & (1u << 22)) r = static_cast<uint64_t>(int64_t(int32_t(m_r[rm])) * int64_t(int32_t(m_r[rs])));
		else r = uint64_t(m_r[rm]) * m_r[rs];
		if (_op & (1u << 21)) r += (uint64_t(m_r[hi]) << 32) | m_r[lo];
		m_r[lo] = uint32_t(r);
		m_r[hi] = uint32_t(r >> 32);
		if (_op & (1u << 20)) m_cpsr = (m_cpsr & ~(N | Z)) | (uint32_t(r >> 32) & N) | (r ? 0 : Z);
	}

	void Arm4::execSwap(const uint32_t _op)
	{
		const uint32_t rn = (_op >> 16) & 0xF, rd = (_op >> 12) & 0xF, rm = _op & 0xF;
		const uint32_t addr = m_r[rn], src = m_r[rm];
		const bool priv = privileged();
		if (_op & (1u << 22))
		{
			uint8_t v;
			if (!load8(addr, v, priv)) return;
			if (!store8(addr, uint8_t(src), priv)) return;
			m_r[rd] = v;
		}
		else
		{
			uint32_t v;
			if (!load32(addr, v, priv)) return;
			if (!store32(addr & ~3u, src, priv)) return;
			m_r[rd] = v;
		}
	}

	void Arm4::execHalfword(const uint32_t _op)
	{
		const bool p = _op & (1u << 24), u = _op & (1u << 23), w = _op & (1u << 21), l = _op & (1u << 20);
		const uint32_t rn = (_op >> 16) & 0xF, rd = (_op >> 12) & 0xF, sh = (_op >> 5) & 3;
		const uint32_t off = (_op & (1u << 22)) ? (((_op >> 4) & 0xF0) | (_op & 0xF)) : m_r[_op & 0xF];
		const uint32_t base = m_r[rn];
		const uint32_t offAddr = u ? base + off : base - off;
		const uint32_t addr = p ? offAddr : base;
		const bool priv = privileged();

		if (l)
		{
			uint32_t v;
			if (sh == 1) { uint16_t h; if (!load16(addr, h, priv)) return; v = h; }
			else if (sh == 2) { uint8_t bt; if (!load8(addr, bt, priv)) return; v = uint32_t(int32_t(int8_t(bt))); }
			else { uint16_t h; if (!load16(addr, h, priv)) return; v = uint32_t(int32_t(int16_t(h))); }
			if (!p || w) m_r[rn] = offAddr;
			if (rd == 15) setPc(v); else m_r[rd] = v;
		}
		else
		{
			if (sh != 1) { undefined(); return; }  // LDRD/STRD do not exist on ARMv4
			const uint32_t v = m_r[rd] + (rd == 15 ? 4 : 0);
			if (!store16(addr, uint16_t(v), priv)) return;
			if (!p || w) m_r[rn] = offAddr;
		}
	}

	void Arm4::execSingle(const uint32_t _op)
	{
		const bool p = _op & (1u << 24), u = _op & (1u << 23), byte = _op & (1u << 22), w = _op & (1u << 21), l = _op & (1u << 20);
		const uint32_t rn = (_op >> 16) & 0xF, rd = (_op >> 12) & 0xF;
		uint32_t off;
		if (_op & (1u << 25)) { bool c; off = shifterOperand(_op & ~(0x10u | (1u << 25)), c); }
		else off = _op & 0xFFF;
		const uint32_t base = m_r[rn];
		const uint32_t offAddr = u ? base + off : base - off;
		const uint32_t addr = p ? offAddr : base;
		const bool priv = privileged() && !(!p && w);  // LDRT/STRT use user permissions

		if (l)
		{
			uint32_t v;
			if (byte) { uint8_t bt; if (!load8(addr, bt, priv)) return; v = bt; }
			else if (!load32(addr, v, priv)) return;
			if (!p || w) m_r[rn] = offAddr;
			if (rd == 15) setPc(v & ~3u); else m_r[rd] = v;
		}
		else
		{
			const uint32_t v = m_r[rd] + (rd == 15 ? 4 : 0);  // STR pc stores current + 12 on StrongARM
			if (byte) { if (!store8(addr, uint8_t(v), priv)) return; }
			else if (!store32(addr & ~3u, v, priv)) return;
			if (!p || w) m_r[rn] = offAddr;
		}
	}

	void Arm4::execBlock(const uint32_t _op)
	{
		const bool p = _op & (1u << 24), u = _op & (1u << 23), s = _op & (1u << 22), w = _op & (1u << 21), l = _op & (1u << 20);
		const uint32_t rn = (_op >> 16) & 0xF, list = _op & 0xFFFF;
		uint32_t n = 0;
		for (int i = 0; i < 16; ++i) n += (list >> i) & 1;
		if (n == 0) return;  // unpredictable; treat as no-op
		const uint32_t base = m_r[rn];
		uint32_t addr = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
		const uint32_t wb = u ? base + 4 * n : base - 4 * n;
		const bool priv = privileged();
		const bool userBank = s && !(l && (list & 0x8000));
		const uint32_t mode = m_cpsr & 0x1F;

		if (l)
		{
			std::array<uint32_t, 16> vals{};
			uint32_t a = addr;
			for (int i = 0; i < 16; ++i)
				if (list & (1u << i)) { if (!load32(a, vals[i], priv)) return; a += 4; }  // base-restored abort
			if (w) m_r[rn] = wb;
			if (userBank && mode != Usr && mode != Sys)
			{
				const uint32_t saved = m_cpsr;
				setCpsr((m_cpsr & ~0x1Fu) | Usr);
				for (int i = 0; i < 15; ++i) if (list & (1u << i)) m_r[i] = vals[i];
				setCpsr(saved);
			}
			else
				for (int i = 0; i < 15; ++i) if (list & (1u << i)) m_r[i] = vals[i];
			if (list & 0x8000)
			{
				if (s) { if (auto* sp = spsrPtr()) setCpsr(*sp); }
				setPc(vals[15] & ~3u);
			}
		}
		else
		{
			const uint32_t saved = m_cpsr;
			if (userBank && mode != Usr && mode != Sys) setCpsr((m_cpsr & ~0x1Fu) | Usr);
			uint32_t a = addr;
			bool ok = true;
			bool first = true;
			for (int i = 0; i < 16 && ok; ++i)
				if (list & (1u << i))
				{
					uint32_t v = m_r[i];
					if (i == 15) v += 4;
					if (static_cast<uint32_t>(i) == rn && w && !first) v = wb;
					ok = store32(a, v, priv);
					a += 4;
					first = false;
				}
			if (m_cpsr != saved) setCpsr(saved);
			if (ok && w) m_r[rn] = wb;
		}
	}

	void Arm4::execMrs(const uint32_t _op)
	{
		const uint32_t rd = (_op >> 12) & 0xF;
		if (_op & (1u << 22)) { auto* sp = spsrPtr(); m_r[rd] = sp ? *sp : m_cpsr; }
		else m_r[rd] = m_cpsr;
	}

	void Arm4::execMsr(const uint32_t _op)
	{
		uint32_t v;
		if (_op & (1u << 25)) v = ror(_op & 0xFF, ((_op >> 8) & 0xF) * 2);
		else v = m_r[_op & 0xF];
		uint32_t mask = 0;
		if (_op & (1u << 16)) mask |= 0x000000FF;
		if (_op & (1u << 17)) mask |= 0x0000FF00;
		if (_op & (1u << 18)) mask |= 0x00FF0000;
		if (_op & (1u << 19)) mask |= 0xFF000000;
		if (_op & (1u << 22))
		{
			if (auto* sp = spsrPtr()) *sp = (*sp & ~mask) | (v & mask);
			return;
		}
		if (!privileged()) mask &= 0xFF000000;
		setCpsr((m_cpsr & ~mask) | (v & mask));
	}

	void Arm4::execCoproc(const uint32_t _op)
	{
		const uint32_t cp = (_op >> 8) & 0xF;
		if (cp != 15 || !privileged()) { undefined(); return; }
		const uint32_t crn = (_op >> 16) & 0xF, rd = (_op >> 12) & 0xF;
		if (_op & (1u << 20))  // MRC
		{
			uint32_t v = 0;
			switch (crn)
			{
			case 0: v = 0x4401A119; break;  // Intel StrongARM SA-1100
			case 1: v = m_ctrl | 0x70; break;
			case 2: v = m_ttb; break;
			case 3: v = m_dacr; break;
			case 5: v = m_fsr; break;
			case 6: v = m_far; break;
			case 13: v = m_pid; break;
			default: v = 0; break;
			}
			if (rd == 15) m_cpsr = (m_cpsr & 0x0FFFFFFF) | (v & 0xF0000000);
			else m_r[rd] = v;
			return;
		}
		const uint32_t v = m_r[rd];
		switch (crn)
		{
		case 1:
			// StrongARM pipeline: the instruction after the MCR that enables the MMU has already been fetched
			// untranslated. Windows CE relies on this ("mcr p15, c1 ; mov pc, rVirtual").
			if (!(m_ctrl & 1) && (v & 1)) m_flatFetch = 1;
			m_ctrl = v;
			flushTlb();
			break;
		case 2: m_ttb = v; flushTlb(); break;
		case 3: m_dacr = v; flushTlb(); break;
		case 5: m_fsr = v; break;
		case 6: m_far = v; break;
		case 8: flushTlb(); break;   // TLB invalidate (all / single entry)
		case 13: m_pid = v & 0xFE000000; m_fetchPageVa = 1; break;
		case 15:
			// SA-1100 c15: CRm=8, opc2=2 = wait for interrupt (idle mode). Others: clock switching, test.
			if ((_op & 0xF) == 8 && ((_op >> 5) & 7) == 2) m_wfi = true;
			break;
		default: break;              // c7 cache ops, c9 read buffer: no effect
		}
	}
}
