// Reverb card ("Chameleon") model: DSP56301 + 2 x Z80 + 4 x Lexichip III + 3 x T-MIX.
// Address decode for the 56301 external bus from the service manual, p. 7-37.
#pragma once

#include "l960/dsp56301.h"
#include "l960/lexichip.h"
#include "l960/z80unit.h"

#include <array>
#include <memory>
#include <vector>

namespace l960
{
	// T-MIX serial audio ASIC. Its internals are undocumented; this is a behavioural stand-in that stores
	// what the 56301 writes so the routing can be reconstructed from traces.
	class Tmix
	{
	public:
		static constexpr uint32_t DspPortWords = 0x400;

		uint32_t dspRead(uint32_t _offset) const { return m_dsp[_offset % DspPortWords]; }
		void dspWrite(uint32_t _offset, uint32_t _v) { m_dsp[_offset % DspPortWords] = _v & 0xFFFFFF; }

		// Control port: write pointer, then data (manual: "Control Pointer Port" / "Control Data Port")
		uint32_t ctlRead(bool _data) const { return _data ? m_ctl[m_ptr & 0xFF] : m_ptr; }
		void ctlWrite(bool _data, uint32_t _v);

		uint8_t reg(uint8_t _i) const { return static_cast<uint8_t>(m_ctl[_i]); }
		void reset() { m_dsp.fill(0); m_ctl.fill(0); m_ptr = 0; }

		// Reset outputs (from ChamDSP.pci): bit 1 of the register is the active-low reset, 4 = hold, 6 = release.
		//   T-MIX #3 $66 / $67 -> Z80 #1 / #2 INITn; T-MIX #2 $61 / $62 -> Lexichip (board half) resets
		static bool released(uint8_t _v) { return (_v & 2) != 0; }

	private:
		std::array<uint32_t, DspPortWords> m_dsp{};
		std::array<uint32_t, 256> m_ctl{};
		uint32_t m_ptr = 0;
	};

	// The 960L's I/O bus as seen from the reverb card's 56301 at X:$301000 (5 slots, $200 apart).
	// Each card has a read-only ID at +0 (type << 4 | revision) and 8-bit control/status latches.
	// Default fit: the standard 960L from the service manual (Table 1-1).
	class IoBus
	{
	public:
		static constexpr uint32_t Slots = 5, SlotSize = 0x200;
		enum CardId : uint8_t { Clock = 0x01, AnalogIn = 0x11, AnalogOut = 0x21, Aes = 0x33, Empty = 0xFF };

		IoBus() { reset(); }
		void reset() { m_regs.fill(0); m_regs[0x001] = 0x02; /* clock card XTAL_EN */ }
		void setCard(uint32_t _slot, uint8_t _id) { if (_slot < Slots) m_ids[_slot] = _id; }
		uint8_t card(uint32_t _slot) const { return _slot < Slots ? m_ids[_slot] : Empty; }

		uint8_t read(uint32_t _offset) const;
		void write(uint32_t _offset, uint8_t _v);

		// Sample rate selected on the clock card (CTLREG b6 double speed, b5:4 WCKSEL); 0 = external/unknown
		uint32_t sampleRate() const;

	private:
		std::array<uint8_t, Slots> m_ids{Clock, Empty, Aes, AnalogIn, AnalogOut};
		std::array<uint8_t, Slots * SlotSize> m_regs{};
	};

	class ReverbCard final : public CardBus
	{
	public:
		ReverbCard();

		void reset();

		// CardBus (56301 external bus)
		uint32_t busRead(uint32_t _addr) override;
		void busWrite(uint32_t _addr, uint32_t _value) override;

		Lexichip& lexichip(size_t _i) { return *m_lex[_i]; }
		Z80Unit& z80(size_t _i) { return *m_z80[_i]; }
		Dpram& dpram(size_t _i) { return m_dpram[_i]; }
		Tmix& tmix(size_t _i) { return m_tmix[_i]; }
		Dsp56301& dsp() { return *m_dsp; }
		IoBus& io() { return m_io; }

		// Steps the control processors for one word clock (Lexichips run separately in the audio path)
		void stepControl(uint32_t _z80Ticks, uint64_t _dspInstructions);

		uint64_t unmappedAccesses() const { return m_unmapped; }

	private:
		std::array<std::unique_ptr<Lexichip>, 4> m_lex;
		std::array<Dpram, 2> m_dpram;
		std::array<std::unique_ptr<Z80Unit>, 2> m_z80;
		std::array<Tmix, 3> m_tmix;
		std::unique_ptr<Dsp56301> m_dsp;
		IoBus m_io;
		uint64_t m_unmapped = 0;
		std::array<bool, 2> m_mailboxIrq{};
	};
}
