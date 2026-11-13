#include "l960/z80unit.h"

#include "chips/z80.h"

namespace l960
{
	// ---------------------------------------------------------------- Dpram

	uint8_t Dpram::readLeft(const uint16_t _a)
	{
		const uint16_t a = _a & (Size - 1);
		if (a == MailboxToZ80) m_intL = false;  // IDT-style mailbox: reading the flag address clears it
		return m_mem[a];
	}

	void Dpram::writeLeft(const uint16_t _a, const uint8_t _v)
	{
		const uint16_t a = _a & (Size - 1);
		m_mem[a] = _v;
		if (a >= MailboxToZ80 && onMailbox) onMailbox('Z', a, _v);
		if (a == MailboxTo56k) m_intR = true;
		if (a == MailboxToZ80) m_intL = false;  // "If the Z80 then writes to the same address, the line returns high"
	}

	uint8_t Dpram::readRight(const uint16_t _a)
	{
		const uint16_t a = _a & (Size - 1);
		if (a == MailboxTo56k) m_intR = false;  // "The interrupt is cleared when this address is read by the 56301"
		return m_mem[a];
	}

	void Dpram::writeRight(const uint16_t _a, const uint8_t _v)
	{
		const uint16_t a = _a & (Size - 1);
		m_mem[a] = _v;
		if (a >= MailboxToZ80 && onMailbox) onMailbox('D', a, _v);
		if (a == MailboxToZ80) m_intL = true;
		if (a == MailboxTo56k) m_intR = false;
	}

	// ---------------------------------------------------------------- Z80Unit

	struct Z80Unit::Cpu
	{
		z80_t z80{};
		uint64_t pins = 0;
	};

	Z80Unit::Z80Unit(Dpram& _dpram, Lexichip& _a, Lexichip& _b)
		: m_cpu(std::make_unique<Cpu>()), m_dpram(_dpram), m_lexA(_a), m_lexB(_b)
	{
		m_cpu->pins = z80_init(&m_cpu->z80);
	}

	Z80Unit::~Z80Unit() = default;

	void Z80Unit::setReset(const bool _held)
	{
		if (m_reset && !_held) m_cpu->pins = z80_reset(&m_cpu->z80);
		m_reset = _held;
	}

	uint16_t Z80Unit::pc() const { return m_cpu->z80.pc; }

	bool Z80Unit::halted() const { return (m_cpu->pins & Z80_HALT) != 0; }

	uint8_t Z80Unit::peek(const uint16_t _addr) { return memRead(_addr); }

	uint8_t Z80Unit::memRead(const uint16_t _a)
	{
		if (_a < 0x0800) return m_dpram.readLeft(_a);
		if (_a >= 0x3000 && _a < 0x3C00)
		{
			// INTL is polled through Lexichip A's status register; keep it current within a slice
			m_lexA.setMailboxInt(m_dpram.intToZ80());
			return m_lexA.busRead(static_cast<uint16_t>(_a - 0x3000));
		}
		if (_a >= 0x4000 && _a < 0x8000) return m_sram[m_bank[0]][_a - 0x4000];
		if (_a >= 0x8000 && _a < 0x8C00) return m_lexB.busRead(static_cast<uint16_t>(_a - 0x8000));
		if (_a >= 0xC000) return m_sram[4 + m_bank[1]][_a - 0xC000];
		return 0xFF;
	}

	void Z80Unit::memWrite(const uint16_t _a, const uint8_t _v)
	{
		if (_a < 0x0800) m_dpram.writeLeft(_a, _v);
		else if (_a >= 0x3000 && _a < 0x3C00)
		{
			// Lexichip A $B7E: SRAM bank select for both 16 KB windows (Z80OS.PCI: LoadBulk, RunSingleFunction)
			if (_a == 0x3B7E) { m_bank[0] = _v & 3; m_bank[1] = _v & 3; }
			m_lexA.busWrite(static_cast<uint16_t>(_a - 0x3000), _v);
		}
		else if (_a >= 0x4000 && _a < 0x8000) m_sram[m_bank[0]][_a - 0x4000] = _v;
		else if (_a >= 0x8000 && _a < 0x8C00) m_lexB.busWrite(static_cast<uint16_t>(_a - 0x8000), _v);
		else if (_a >= 0xC000) m_sram[4 + m_bank[1]][_a - 0xC000] = _v;
		else if (onUnmappedWrite) onUnmappedWrite(_a, _v);
	}

	void Z80Unit::run(const uint32_t _ticks)
	{
		if (m_reset) return;
		// INTL is polled by the Z80 through Lexichip A's status register rather than wired to /INT
		m_lexA.setMailboxInt(m_dpram.intToZ80());

		auto& pins = m_cpu->pins;
		for (uint32_t t = 0; t < _ticks; ++t)
		{
			pins = z80_tick(&m_cpu->z80, pins);
			if (pins & Z80_MREQ)
			{
				const uint16_t addr = Z80_GET_ADDR(pins);
				if (pins & Z80_RD) { const uint8_t d = memRead(addr); Z80_SET_DATA(pins, d); }
				else if (pins & Z80_WR) memWrite(addr, Z80_GET_DATA(pins));
			}
			else if (pins & Z80_IORQ)
			{
				// No I/O devices are documented on the Z80 bus; return open bus
				if ((pins & Z80_RD) || (pins & Z80_M1)) Z80_SET_DATA(pins, 0xFF);
			}
		}
		m_ticks += _ticks;
	}
}
