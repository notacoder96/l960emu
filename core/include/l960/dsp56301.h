// DSP56301 wrapper around the dsp56300 emulator core (github.com/dsp56300/dsp56300, GPL-3).
//
// The 56301 differs from the 56303/56362 that dsp56300 models mainly in its peripherals: it has the
// HI32 PCI host interface instead of HI08/HDI08. dsp56300 is patched (patches/) to accept host-defined
// peripherals; this class implements the parts of the HI32 that the 960L firmware uses:
//   DSP side  X:$FFFFC5 DCTR, $C6 DPCR, $C7 DPMC, $C8 DPAR, $C9 DSR, $CA DPSR, $CB DRXR, $CC DTXM, $CD DTXS
//   host side HCTR, HSTR, HCVR (host commands), HTXR -> DRXR and DTXS -> HRXS slave FIFOs
//   PCI master transfers (DPMC/DPAR + DMA channel 0), done atomically against pciRead/pciWrite
// The bootstrap ROM ($FF0000) is not available; bootstrap() loads an image the way it would.
//
// External bus (service manual p. 7-37) is routed through CardBus, implemented by the card model.
#pragma once

#include "l960/firmware.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace l960
{
	class CardBus
	{
	public:
		virtual ~CardBus() = default;
		virtual uint32_t busRead(uint32_t _addr) = 0;
		virtual void busWrite(uint32_t _addr, uint32_t _value) = 0;
	};

	struct PeripheralAccess
	{
		uint32_t reads = 0, writes = 0, lastValue = 0;
	};

	class Dsp56301
	{
	public:
		// Start of the external decodes in the 56301 memory map (I/O bus slots at $301000)
		static constexpr uint32_t ExternalBegin = 0x300000;
		static constexpr uint32_t ExternalEnd = 0xD00400;

		// HI32 host-side register offsets in PCI BAR0 (as used by CHAM.sys)
		static constexpr uint32_t HCTR = 0x10, HSTR = 0x14, HCVR = 0x18, HTXR_HRXS = 0x1C;

		explicit Dsp56301(CardBus& _bus);
		~Dsp56301();

		static bool available();

		// Writes a boot image into P memory and points the PC at its load address
		void load(const Dsp56kBootImage& _img);
		// Hardware reset followed by the HI32 bootstrap of _img (what the boot ROM does with the host stream)
		void bootstrap(const Dsp56kBootImage& _img);

		void run(uint64_t _instructions);

		// Debugging: called when the PC lands on trapAddress (0 = off) with the previous PC and a register dump
		uint32_t trapAddress = 0;
		std::function<void(uint32_t prevPc, const std::string& regs)> onTrap;
		void irq(char _line);  // 'A' word clock, 'C'/'D' Z80 mailboxes (only taken when enabled in IPRC)

		// Word-clock period measurement (timers 0 and 1 in capture mode, 40 MHz timer clock as the host assumes)
		void timerCapture(uint32_t _period0, uint32_t _period1);

		uint32_t pc() const;
		uint64_t instructions() const;
		uint32_t readP(uint32_t _addr) const;
		uint32_t readX(uint32_t _addr) const;
		uint32_t readY(uint32_t _addr) const;
		void writeMem(int _space, uint32_t _addr, uint32_t _value);  // 0 X, 1 Y, 2 P

		// X:$FFFF80.. register accesses seen so far
		const std::map<uint32_t, PeripheralAccess>& peripheralLog() const;
		std::function<void(uint32_t addr, uint32_t value)> onPeripheralWrite;

		// --- HI32 host side (PCI target). Call from the thread that runs the DSP.
		void hostWrite(uint32_t _offset, uint32_t _value);
		uint32_t hostRead(uint32_t _offset);
		bool hostInterrupt() const;       // INTA# = DCTR.HINT
		size_t hostRxCount() const;       // words waiting in HRXS
		size_t hostTxPending() const;     // HTXR words not yet read by the DSP
		uint64_t hostInterruptAcks() const;  // times the DSP cleared DCTR.HINT

		// --- HI32 PCI master: 32-bit host memory accesses (byte addresses)
		std::function<uint32_t(uint32_t addr)> pciRead;
		std::function<void(uint32_t addr, uint32_t value)> pciWrite;
		uint64_t masterTransfers() const;

		struct Impl;  // public so the peripheral model in the .cpp can reach it

	private:
		std::unique_ptr<Impl> m;
	};
}
