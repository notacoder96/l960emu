// ARMv4 (StrongARM SA-1100) CPU interpreter with the SA-1100 CP15 MMU.
//
// Implements: all ARMv4 ARM-state instructions (no Thumb: StrongARM is ARMv4 without T), long
// multiplies, halfword/signed transfers, SWP, MRS/MSR, CP15 MRC/MCR; the SA-1100 MMU (sections,
// coarse and fine second-level tables, large/small/tiny pages, domains, AP with S/R bits), the
// SA-1100 process ID register (c13: VA[31:25]==0 is relocated by PID) and the base-restored data
// abort model. Caches and write buffers are not modelled (cache maintenance ops are no-ops).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace l960::larc2
{
	// Physical bus. Return false for an external abort.
	class ArmBus
	{
	public:
		virtual ~ArmBus() = default;
		virtual bool read32(uint32_t _pa, uint32_t& _v) = 0;
		virtual bool read16(uint32_t _pa, uint16_t& _v) = 0;
		virtual bool read8(uint32_t _pa, uint8_t& _v) = 0;
		virtual bool write32(uint32_t _pa, uint32_t _v) = 0;
		virtual bool write16(uint32_t _pa, uint16_t _v) = 0;
		virtual bool write8(uint32_t _pa, uint8_t _v) = 0;
		// Fast path for instruction fetch: pointer to host memory backing the 4 KB page, or nullptr
		virtual const uint32_t* fetchPage(uint32_t _pa) { (void)_pa; return nullptr; }
	};

	class Arm4
	{
	public:
		enum Mode : uint32_t { Usr = 0x10, Fiq = 0x11, Irq = 0x12, Svc = 0x13, Abt = 0x17, Und = 0x1B, Sys = 0x1F };

		explicit Arm4(ArmBus& _bus);

		void reset();
		// Runs up to _instructions; returns the number executed
		uint64_t run(uint64_t _instructions);

		void setIrqLine(bool _asserted) { m_irqLine = _asserted; }
		void setFiqLine(bool _asserted) { m_fiqLine = _asserted; }
		// SA-1100 "wait for interrupt" (MCR p15, 0, rX, c15, c8, 2)
		bool waitingForInterrupt() const { return m_wfi; }
		void wakeIfInterrupt() { if (m_irqLine || m_fiqLine) m_wfi = false; }

		uint32_t reg(int _i) const { return m_r[_i]; }
		uint32_t pc() const { return m_r[15]; }
		uint32_t cpsr() const { return m_cpsr; }
		uint64_t instructions() const { return m_instr; }
		uint32_t cp15Control() const { return m_ctrl; }

		// Debug access through the MMU with current privilege (false on fault)
		bool debugRead32(uint32_t _va, uint32_t& _v);

		struct Stats { uint64_t undefined = 0, swi = 0, prefetchAborts = 0, dataAborts = 0, irqs = 0, fiqs = 0; };
		const Stats& stats() const { return m_stats; }
		uint32_t faultStatus() const { return m_fsr; }
		// Ring buffer of the last executed PCs (debug)
		std::array<uint32_t, 64> pcTrace{};
		uint32_t pcTracePos = 0;
		// Debug hook: vector offset, address of the faulting/next instruction, FAR, FSR
		std::function<void(uint32_t vector, uint32_t pc, uint32_t far, uint32_t fsr)> onException;
		uint32_t lastAbortAddress() const { return m_far; }

	private:
		enum class Access { Read, Write, Fetch };
		struct TlbEntry { uint32_t tag; uint32_t paPage; uint8_t perm; uint32_t gen; };  // perm bits: privR privW usrR usrW

		// execution
		void step();
		bool cond(uint32_t _c) const;
		void execDataProc(uint32_t _op);
		void execMultiply(uint32_t _op);
		void execMultiplyLong(uint32_t _op);
		void execSwap(uint32_t _op);
		void execHalfword(uint32_t _op);
		void execSingle(uint32_t _op);
		void execBlock(uint32_t _op);
		void execMrs(uint32_t _op);
		void execMsr(uint32_t _op);
		void execCoproc(uint32_t _op);
		uint32_t shifterOperand(uint32_t _op, bool& _carry);

		// exceptions / modes
		void exception(uint32_t _vectorOffset, Mode _mode, uint32_t _lr, bool _disableFiq);
		void undefined();
		void setCpsr(uint32_t _v);
		void switchBank(uint32_t _oldMode, uint32_t _newMode);
		uint32_t* spsrPtr();
		bool privileged() const { return (m_cpsr & 0x1F) != Usr; }
		void setPc(uint32_t _v) { m_r[15] = _v; m_branched = true; }

		// memory
		uint32_t mva(uint32_t _va) const { return (_va & 0xFE000000) ? _va : (_va | m_pid); }
		bool translate(uint32_t _va, Access _acc, bool _priv, uint32_t& _pa);
		bool walk(uint32_t _mva, Access _acc, bool _priv, uint32_t& _pa, TlbEntry* _fill);
		void fault(uint32_t _status, uint32_t _domain, uint32_t _mva, Access _acc);
		bool load32(uint32_t _va, uint32_t& _v, bool _priv);
		bool load16(uint32_t _va, uint16_t& _v, bool _priv);
		bool load8(uint32_t _va, uint8_t& _v, bool _priv);
		bool store32(uint32_t _va, uint32_t _v, bool _priv);
		bool store16(uint32_t _va, uint16_t _v, bool _priv);
		bool store8(uint32_t _va, uint8_t _v, bool _priv);
		bool fetch(uint32_t _va, uint32_t& _op);
		void flushTlb() { ++m_tlbGen; m_fetchPageVa = 1; }

		ArmBus& m_bus;
		std::array<uint32_t, 16> m_r{};
		uint32_t m_cpsr = 0;
		// banked registers: [0]=usr/sys, then fiq, irq, svc, abt, und
		std::array<std::array<uint32_t, 7>, 6> m_bank{};   // r8..r14 (fiq uses all 7, others r13/r14)
		std::array<uint32_t, 6> m_spsr{};

		// CP15
		uint32_t m_ctrl = 0, m_ttb = 0, m_dacr = 0, m_fsr = 0, m_far = 0, m_pid = 0;

		std::vector<TlbEntry> m_tlb;
		uint32_t m_tlbGen = 1;
		uint32_t m_fetchPageVa = 1;  // cached instruction page (VA, priv-tagged), 1 = invalid
		const uint32_t* m_fetchHost = nullptr;
		uint32_t m_fetchPa = 0;
		bool m_fetchPriv = false;

		bool m_irqLine = false, m_fiqLine = false;
		bool m_aborted = false;  // set by a data abort during the current instruction
		bool m_branched = false; // r15 written during the current instruction
		int m_flatFetch = 0;
		bool m_wfi = false;      // instructions still fetched untranslated after the MMU was switched on
		uint64_t m_instr = 0;
		Stats m_stats;
	};
}
