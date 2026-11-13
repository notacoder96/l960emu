#include "l960/dsp56301.h"

#if L960_HAVE_DSP56300

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/externalbusdevice.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <array>
#include <deque>
#include <sstream>

namespace l960
{
	namespace
	{
		using dsp56k::TWord;

		// X: I/O addresses
		enum : TWord
		{
			A_DCTR = 0xFFFFC5, A_DPCR = 0xFFFFC6, A_DPMC = 0xFFFFC7, A_DPAR = 0xFFFFC8, A_DSR = 0xFFFFC9,
			A_DPSR = 0xFFFFCA, A_DRXR = 0xFFFFCB, A_DTXM = 0xFFFFCC, A_DTXS = 0xFFFFCD,
			A_DCR0 = 0xFFFFEC, A_DCO0 = 0xFFFFED, A_DDR0 = 0xFFFFEE, A_DSR0 = 0xFFFFEF, A_DSTR = 0xFFFFF4,
			A_IDR = 0xFFFFF5, A_IPRP = 0xFFFFFE, A_IPRC = 0xFFFFFF,
		};

		// DCTR / DSR / HSTR bits
		constexpr TWord DCTR_HCIE = 1u << 0, DCTR_HINT = 1u << 6, DCTR_HF_MASK = 7u << 3;
		constexpr TWord DSR_HCP = 1u << 0, DSR_STRQ = 1u << 1, DSR_SRRQ = 1u << 2;
		constexpr uint32_t HSTR_TRDY = 1u << 0, HSTR_HTRQ = 1u << 1, HSTR_HRRQ = 1u << 2, HSTR_HINT = 1u << 6;
		constexpr uint32_t HCVR_HC = 1u << 0, HCVR_HNMI = 1u << 15;
		constexpr TWord DPSR_MARQ = 1u << 4, DPSR_DONE = 1u << 14;
		constexpr TWord DCR_DE = 1u << 23, DCR_DIE = 1u << 22;
		constexpr size_t SlaveTxDepth = 8;  // HI32 DSP-to-host slave FIFO (words)

		constexpr TWord idx(TWord _a) { return (_a - dsp56k::XIO_Reserved_High_First) & 127; }
	}

	struct Dsp56301::Impl;

	namespace
	{
		class Hi32Peripherals final : public dsp56k::PeripheralsGeneric
		{
		public:
			std::array<TWord, 128> regs{};
			std::map<uint32_t, PeripheralAccess> log;
			Dsp56301::Impl* owner = nullptr;

			Hi32Peripherals() { Hi32Peripherals::reset(); }

			TWord read(TWord _addr, dsp56k::Instruction) override;
			const TWord* readAsPtr(TWord, dsp56k::Instruction) override { return nullptr; }  // reads have side effects
			void write(TWord _addr, TWord _value) override;

			void reset() override
			{
				regs.fill(0);
				// "56301 IDR . . . 0x2301" in the 960L reverb-card diagnostics (service manual, chapter 6)
				regs[idx(A_IDR)] = 0x002301;
			}
			void setSymbols(dsp56k::Disassembler&) const override {}
			void terminate() override {}
			uint32_t execGeneric() noexcept override { return 64; }
		};

		class ExternalBus final : public dsp56k::IExternalBusDevice
		{
		public:
			explicit ExternalBus(CardBus& _b) : bus(_b) {}
			TWord read(const TWord _addr) override { return bus.busRead(_addr) & 0xFFFFFF; }
			void write(const TWord _addr, const TWord _value) override { bus.busWrite(_addr, _value & 0xFFFFFF); }
			CardBus& bus;
		};
	}

	struct Dsp56301::Impl
	{
		explicit Impl(Dsp56301& _self, CardBus& _bus)
			: self(_self), mem(validator, 0x1000000), ext(_bus), dsp(mem, &periphX, &periphY)
		{
			periphX.owner = this;
			auto cfg = dsp.getJit().getConfig();
			cfg.externalBusBegin = ExternalBegin;
			cfg.externalBusEnd = ExternalEnd;
			dsp.getJit().setConfig(cfg);
			dsp.setExternalBusDevice(&ext);
		}

		TWord& reg(TWord _a) { return periphX.regs[idx(_a)]; }

		// ---- slave FIFOs and host commands
		std::deque<uint32_t> toDsp;     // HTXR -> DRXR
		std::deque<uint32_t> toHost;    // DTXS -> HRXS
		uint32_t hctr = 0, hcvr = 0;
		bool dparArmed = false;
		uint64_t masterCount = 0, hintAcks = 0;

		void tryHostCommand()
		{
			if (!(hcvr & HCVR_HC)) return;
			if (!(reg(A_DCTR) & DCTR_HCIE) && !(hcvr & HCVR_HNMI)) return;
			hcvr &= ~HCVR_HC;
			dsp.injectInterrupt(hcvr & 0xFE);
		}

		void tryMaster()
		{
			if (!dparArmed || !(reg(A_DCR0) & DCR_DE)) return;
			dparArmed = false;
			const TWord dpar = reg(A_DPAR), dpmc = reg(A_DPMC), dcr = reg(A_DCR0);
			const uint32_t addr = ((dpmc & 0xFFFF) << 16) | (dpar & 0xFFFF);
			const uint32_t cmd = (dpar >> 16) & 0xF;
			const uint32_t n = (reg(A_DCO0) & 0xFFFF) + 1;
			const auto space = [](TWord _bits) { return _bits == 1 ? dsp56k::MemArea_Y : _bits == 2 ? dsp56k::MemArea_P : dsp56k::MemArea_X; };
			if (cmd & 1)  // memory write: DSP -> host
			{
				const auto src = space(dcr & 3);
				TWord a = reg(A_DSR0);
				for (uint32_t i = 0; i < n; ++i, ++a)
					if (self.pciWrite) self.pciWrite(addr + 4 * i, mem.get(src, a) & 0xFFFFFF);
				reg(A_DSR0) = a & 0xFFFFFF;
			}
			else  // memory read: host -> DSP
			{
				const auto dst = space((dcr >> 2) & 3);
				TWord a = reg(A_DDR0);
				for (uint32_t i = 0; i < n; ++i, ++a)
					mem.set(dst, a, self.pciRead ? self.pciRead(addr + 4 * i) & 0xFFFFFF : 0);
				reg(A_DDR0) = a & 0xFFFFFF;
			}
			++masterCount;
			reg(A_DCO0) = 0;
			reg(A_DCR0) = dcr & ~DCR_DE;
			reg(A_DSTR) |= 1;  // DTD0
			reg(A_DPSR) = (reg(A_DPSR) & ~0x3FFFE0u) | DPSR_MARQ | DPSR_DONE;
			if (dcr & DCR_DIE) dsp.injectInterrupt(dsp56k::Vba_DMAchannel0);
		}

		Dsp56301& self;
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory mem;
		Hi32Peripherals periphX;
		dsp56k::PeripheralsNop periphY;
		ExternalBus ext;
		dsp56k::DSP dsp;
		std::function<void(uint32_t, uint32_t)>* onWrite = nullptr;
	};

	namespace
	{
		TWord Hi32Peripherals::read(const TWord _addr, dsp56k::Instruction)
		{
			++log[_addr].reads;
			if (!owner) return regs[idx(_addr)];
			auto& o = *owner;
			switch (_addr)
			{
			case A_DSR:
			{
				TWord v = (o.hctr & 0x38);  // HF0..HF2 from the host
				if (o.hcvr & HCVR_HC) v |= DSR_HCP;
				if (o.toHost.size() < SlaveTxDepth) v |= DSR_STRQ;
				if (!o.toDsp.empty()) v |= DSR_SRRQ;
				return v;
			}
			case A_DRXR:
			{
				if (o.toDsp.empty()) return regs[idx(_addr)];
				const TWord v = o.toDsp.front() & 0xFFFFFF;
				o.toDsp.pop_front();
				regs[idx(_addr)] = v;
				return v;
			}
			case A_DPSR:
				return regs[idx(_addr)] | DPSR_MARQ;
			case A_DSTR:
				return regs[idx(_addr)] | ((regs[idx(A_DCR0)] & DCR_DE) ? 0 : 1);
			default:
				return regs[idx(_addr)];
			}
		}

		void Hi32Peripherals::write(const TWord _addr, const TWord _value)
		{
			auto& l = log[_addr];
			++l.writes;
			l.lastValue = _value;
			if (!owner) { regs[idx(_addr)] = _value; return; }
			auto& o = *owner;
			switch (_addr)
			{
			case A_DTXS:
				o.toHost.push_back(_value & 0xFFFFFF);
				break;
			case A_DSR: case A_IDR:
				break;  // read-only
			case A_DPAR:
				regs[idx(_addr)] = _value;
				o.dparArmed = true;
				regs[idx(A_DPSR)] &= ~DPSR_DONE;
				o.tryMaster();
				break;
			case A_DCR0:
				regs[idx(_addr)] = _value;
				o.tryMaster();
				break;
			case A_DCTR:
				if ((regs[idx(_addr)] & DCTR_HINT) && !(_value & DCTR_HINT)) ++o.hintAcks;
				regs[idx(_addr)] = _value;
				o.tryHostCommand();
				break;
			default:
				regs[idx(_addr)] = _value;
				break;
			}
			if (o.onWrite && *o.onWrite) (*o.onWrite)(_addr, _value);
		}
	}

	Dsp56301::Dsp56301(CardBus& _bus) : m(std::make_unique<Impl>(*this, _bus))
	{
		m->onWrite = &onPeripheralWrite;
	}

	Dsp56301::~Dsp56301() = default;

	bool Dsp56301::available() { return true; }

	void Dsp56301::load(const Dsp56kBootImage& _img)
	{
		for (size_t i = 0; i < _img.words.size(); ++i)
			m->mem.set(dsp56k::MemArea_P, static_cast<TWord>(_img.loadAddress + i), _img.words[i] & 0xFFFFFF);
		// direct memory writes bypass the JIT's self-modifying-code tracking: drop every compiled block
		m->dsp.getJit().destroyAllBlocks();
		m->dsp.clearOpcodeCache();
		m->dsp.setPC(_img.loadAddress);
	}

	void Dsp56301::bootstrap(const Dsp56kBootImage& _img)
	{
		m->dsp.resetHW();
		m->toDsp.clear();
		m->toHost.clear();
		m->hcvr = 0;
		m->dparArmed = false;
		load(_img);
	}

	void Dsp56301::run(const uint64_t _instructions)
	{
		if (trapAddress)
		{
			for (uint64_t i = 0; i < _instructions; ++i)
			{
				const TWord prev = m->dsp.getPC().toWord();
				m->dsp.exec();
				if (m->dsp.getPC().toWord() == trapAddress && onTrap)
				{
					std::stringstream ss;
					m->dsp.dumpRegisters(ss);
					onTrap(prev, ss.str());
				}
			}
			return;
		}
		for (uint64_t i = 0; i < _instructions; ++i) m->dsp.exec();
	}

	void Dsp56301::irq(const char _line)
	{
		// IPRC: IAL bits 0-2, IBL 3-5, ICL 6-8, IDL 9-11 (2 priority bits + trigger mode); level 0 = disabled
		const int n = _line - 'A';
		if (n < 0 || n > 3) return;
		if (!((m->reg(A_IPRC) >> (3 * n)) & 3)) return;
		m->dsp.injectInterrupt(static_cast<TWord>(0x10 + 2 * n));
	}

	void Dsp56301::timerCapture(const uint32_t _period0, const uint32_t _period1)
	{
		// Timers 0/1 in input-period mode: TCR = measured period, compare interrupt ($24/$28) when TE and TCIE are set
		constexpr TWord TCSR0 = 0xFFFF8F, TCR0 = 0xFFFF8C, TCSR1 = 0xFFFF8B, TCR1 = 0xFFFF88;
		const auto fire = [&](TWord _csr, TWord _cr, uint32_t _p, TWord _vba) {
			const TWord csr = m->reg(_csr);
			if (!_p || (csr & 5) != 5) return;
			m->reg(_cr) = _p & 0xFFFFFF;
			m->reg(_csr) = csr & ~(1u << 20);
			m->dsp.injectInterrupt(_vba);
		};
		fire(TCSR0, TCR0, _period0, 0x24);
		fire(TCSR1, TCR1, _period1, 0x28);
	}

	uint32_t Dsp56301::pc() const { return m->dsp.getPC().toWord(); }
	uint64_t Dsp56301::instructions() const { return m->dsp.getInstructionCounter(); }
	uint32_t Dsp56301::readP(const uint32_t _a) const { return m->mem.get(dsp56k::MemArea_P, _a); }
	uint32_t Dsp56301::readX(const uint32_t _a) const { return m->mem.get(dsp56k::MemArea_X, _a); }
	uint32_t Dsp56301::readY(const uint32_t _a) const { return m->mem.get(dsp56k::MemArea_Y, _a); }

	void Dsp56301::writeMem(const int _space, const uint32_t _a, const uint32_t _v)
	{
		const auto area = _space == 1 ? dsp56k::MemArea_Y : _space == 2 ? dsp56k::MemArea_P : dsp56k::MemArea_X;
		m->mem.set(area, _a, _v & 0xFFFFFF);
		if (area == dsp56k::MemArea_P) m->dsp.clearOpcodeCache(_a);
	}

	const std::map<uint32_t, PeripheralAccess>& Dsp56301::peripheralLog() const { return m->periphX.log; }

	void Dsp56301::hostWrite(const uint32_t _offset, const uint32_t _value)
	{
		switch (_offset)
		{
		case HCTR: m->hctr = _value; break;
		case HCVR:
			m->hcvr = _value & 0x80FF;
			m->tryHostCommand();
			break;
		case HTXR_HRXS: m->toDsp.push_back(_value & 0xFFFFFF); break;
		default: break;
		}
	}

	uint32_t Dsp56301::hostRead(const uint32_t _offset)
	{
		switch (_offset)
		{
		case HCTR: return m->hctr;
		case HCVR: return m->hcvr;
		case HSTR:
		{
			const TWord dctr = m->reg(A_DCTR);
			uint32_t v = (dctr & DCTR_HF_MASK) | HSTR_HTRQ;
			if (m->toDsp.empty()) v |= HSTR_TRDY;
			if (!m->toHost.empty()) v |= HSTR_HRRQ;
			if (dctr & DCTR_HINT) v |= HSTR_HINT;
			return v;
		}
		case HTXR_HRXS:
		{
			if (m->toHost.empty()) return 0;
			const uint32_t v = m->toHost.front();
			m->toHost.pop_front();
			return v;
		}
		default: return 0;
		}
	}

	bool Dsp56301::hostInterrupt() const { return (m->periphX.regs[idx(A_DCTR)] & DCTR_HINT) != 0; }
	size_t Dsp56301::hostRxCount() const { return m->toHost.size(); }
	size_t Dsp56301::hostTxPending() const { return m->toDsp.size(); }
	uint64_t Dsp56301::masterTransfers() const { return m->masterCount; }
	uint64_t Dsp56301::hostInterruptAcks() const { return m->hintAcks; }
}

#else  // built without dsp56300

namespace l960
{
	struct Dsp56301::Impl {};
	Dsp56301::Dsp56301(CardBus&) {}
	Dsp56301::~Dsp56301() = default;
	bool Dsp56301::available() { return false; }
	void Dsp56301::load(const Dsp56kBootImage&) {}
	void Dsp56301::bootstrap(const Dsp56kBootImage&) {}
	void Dsp56301::run(uint64_t) {}
	void Dsp56301::irq(char) {}
	void Dsp56301::timerCapture(uint32_t, uint32_t) {}
	uint32_t Dsp56301::pc() const { return 0; }
	uint64_t Dsp56301::instructions() const { return 0; }
	uint32_t Dsp56301::readP(uint32_t) const { return 0; }
	uint32_t Dsp56301::readX(uint32_t) const { return 0; }
	uint32_t Dsp56301::readY(uint32_t) const { return 0; }
	void Dsp56301::writeMem(int, uint32_t, uint32_t) {}
	const std::map<uint32_t, PeripheralAccess>& Dsp56301::peripheralLog() const
	{
		static const std::map<uint32_t, PeripheralAccess> empty;
		return empty;
	}
	void Dsp56301::hostWrite(uint32_t, uint32_t) {}
	uint32_t Dsp56301::hostRead(uint32_t) { return 0; }
	bool Dsp56301::hostInterrupt() const { return false; }
	size_t Dsp56301::hostRxCount() const { return 0; }
	size_t Dsp56301::hostTxPending() const { return 0; }
	uint64_t Dsp56301::masterTransfers() const { return 0; }
	uint64_t Dsp56301::hostInterruptAcks() const { return 0; }
}

#endif
