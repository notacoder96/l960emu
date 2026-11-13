#include "l960/card.h"

namespace l960
{
	void Tmix::ctlWrite(const bool _data, const uint32_t _v)
	{
		if (!_data) { m_ptr = _v & 0xFF; return; }
		m_ctl[m_ptr & 0xFF] = _v & 0xFF;
		m_ptr = (m_ptr + 1) & 0xFF;  // auto-increment: map rows are written as pointer + 16 data bytes
	}

	uint8_t IoBus::read(const uint32_t _o) const
	{
		const uint32_t slot = _o / SlotSize, r = _o % SlotSize;
		if (slot >= Slots || m_ids[slot] == Empty) return 0xFF;  // pull-ups on an empty slot
		if (r == 0) return m_ids[slot];
		return m_regs[_o];
	}

	void IoBus::write(const uint32_t _o, const uint8_t _v)
	{
		const uint32_t slot = _o / SlotSize;
		if (slot < Slots && m_ids[slot] != Empty && _o % SlotSize) m_regs[_o] = _v;
	}

	uint32_t IoBus::sampleRate() const
	{
		for (uint32_t s = 0; s < Slots; ++s)
		{
			if (m_ids[s] >> 4 != 0) continue;
			const uint8_t ctl = m_regs[s * SlotSize + 1];
			const uint32_t base = ((ctl >> 4) & 3) == 1 ? 44100 : ((ctl >> 4) & 3) == 0 ? 48000 : 0;
			return (ctl & 0x40) ? base * 2 : base;
		}
		return 0;
	}

	ReverbCard::ReverbCard()
	{
		for (auto& l : m_lex) l = std::make_unique<Lexichip>();
		m_z80[0] = std::make_unique<Z80Unit>(m_dpram[0], *m_lex[0], *m_lex[1]);
		m_z80[1] = std::make_unique<Z80Unit>(m_dpram[1], *m_lex[2], *m_lex[3]);
		m_dsp = std::make_unique<Dsp56301>(*this);
	}

	void ReverbCard::reset()
	{
		for (auto& l : m_lex) l->reset();
		for (auto& d : m_dpram) d.reset();
		for (auto& z : m_z80) z->setReset(true);
		for (auto& t : m_tmix) t.reset();
		m_mailboxIrq = {};
	}

	// 56301 expansion port memory map, service manual p. 7-37
	uint32_t ReverbCard::busRead(const uint32_t _a)
	{
		if (_a >= 0x9D0000 && _a < 0x9D0800) return m_dpram[0].readRight(static_cast<uint16_t>(_a - 0x9D0000));
		if (_a >= 0x9D1000 && _a < 0x9D1800) return m_dpram[1].readRight(static_cast<uint16_t>(_a - 0x9D1000));
		if (_a == 0x9D2000 || _a == 0x9D2001) return m_tmix[0].ctlRead(_a & 1);
		if (_a == 0x9D3000 || _a == 0x9D3001) return m_tmix[1].ctlRead(_a & 1);
		if (_a == 0x9D4000 || _a == 0x9D4001) return m_tmix[2].ctlRead(_a & 1);
		if (_a >= 0x83C000 && _a < 0x83C400) return m_tmix[0].dspRead(_a - 0x83C000);
		if (_a >= 0x83A000 && _a < 0x83A400) return m_tmix[1].dspRead(_a - 0x83A000);
		if (_a >= 0x838000 && _a < 0x838400) return m_tmix[2].dspRead(_a - 0x838000);
		if (_a >= 0x301000 && _a < 0x302000) return m_io.read(_a - 0x301000);
		++m_unmapped;
		return 0;
	}

	void ReverbCard::busWrite(const uint32_t _a, const uint32_t _v)
	{
		const auto byte = static_cast<uint8_t>(_v);
		if (_a >= 0x9D0000 && _a < 0x9D0800) m_dpram[0].writeRight(static_cast<uint16_t>(_a - 0x9D0000), byte);
		else if (_a >= 0x9D1000 && _a < 0x9D1800) m_dpram[1].writeRight(static_cast<uint16_t>(_a - 0x9D1000), byte);
		else if (_a == 0x9D2000 || _a == 0x9D2001) m_tmix[0].ctlWrite(_a & 1, _v);
		else if (_a == 0x9D3000 || _a == 0x9D3001) m_tmix[1].ctlWrite(_a & 1, _v);
		else if (_a == 0x9D4000 || _a == 0x9D4001) m_tmix[2].ctlWrite(_a & 1, _v);
		else if (_a >= 0x83C000 && _a < 0x83C400) m_tmix[0].dspWrite(_a - 0x83C000, _v);
		else if (_a >= 0x83A000 && _a < 0x83A400) m_tmix[1].dspWrite(_a - 0x83A000, _v);
		else if (_a >= 0x838000 && _a < 0x838400) m_tmix[2].dspWrite(_a - 0x838000, _v);
		else if (_a >= 0x301000 && _a < 0x302000) m_io.write(_a - 0x301000, byte);
		else ++m_unmapped;
	}

	void ReverbCard::stepControl(const uint32_t _z80Ticks, const uint64_t _dspInstructions)
	{
		// Z80_INITn lines from T-MIX #3 (manual p. 7-35; register numbers from ChamDSP.pci)
		m_z80[0]->setReset(!Tmix::released(m_tmix[2].reg(0x66)));
		m_z80[1]->setReset(!Tmix::released(m_tmix[2].reg(0x67)));

		for (auto& z : m_z80) z->run(_z80Ticks);
		if (m_dsp->available())
		{
			// DB_MAS_INT -> IRQC / IRQD, taken on the rising edge
			for (int i = 0; i < 2; ++i)
			{
				const bool v = m_dpram[i].intTo56k();
				if (v && !m_mailboxIrq[i]) m_dsp->irq(i ? 'D' : 'C');
				m_mailboxIrq[i] = v;
			}
			m_dsp->run(_dspInstructions);
		}
	}
}
