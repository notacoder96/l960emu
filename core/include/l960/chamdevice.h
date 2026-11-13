// Virtual CHAM.sys: a re-implementation of the 960L's NT4 reverb-card driver on top of the emulated card.
//
// The original host software (ChamAPI.dll) talks to \\.\CHAM with ten buffered IOCTLs. CHAM.sys turns
// them into HI32 register accesses and command queues in a 4 KB PCI common buffer that the 56301
// firmware (ChamDSP.pci) reads and writes by PCI bus-master DMA. This class does the same against
// ReverbCard's emulated 56301, so the real firmware runs the real commands. Reverse-engineered from
// CHAM.sys (see doc "Emulator status"), one card supported.
//
// IOCTLs that carry user-mode handles or pointers (0x60 RegisterEvent, 0x64 LockBuffer, and the status
// pointer of 0x5C) are completed in the caller's process; results that the driver would deliver
// asynchronously (event signals, readback buffers) are returned with each call.
#pragma once

#include "l960/card.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace l960
{
	class ChamDevice
	{
	public:
		enum Ioctl : uint32_t
		{
			GetConfiguration = 0xA410EC40,
			Download56KCode = 0xA410EC44,
			HostCommand = 0xA410EC48,
			TmixCommand = 0xA410EC4C,
			Z80Command = 0xA410EC50,
			DownloadZ80Kernel = 0xA410EC54,
			InitializeQueues = 0xA410EC58,
			GetPeriodicStatus = 0xA410EC5C,
			RegisterEvent = 0xA410EC60,
			LockBuffer = 0xA410EC64,
		};

		struct Readback
		{
			uint32_t mdl = 0;              // slot from LockBuffer
			std::vector<uint8_t> bytes;    // what the driver copies into the locked buffer
		};

		struct Result
		{
			bool ok = true;                // IRP status success
			uint32_t info = 0;             // IRP Information: bytes returned, or an ERRVAL for ChamAPI
			std::vector<uint8_t> out;
			std::vector<uint32_t> events;  // event slots to signal
			std::vector<Readback> readbacks;
		};

		static constexpr uint32_t CommonBufferPhys = 0x00200000;
		static constexpr uint32_t CommonBufferSize = 0x1000;

		explicit ChamDevice(ReverbCard& _card);

		// Needs <root>/Cham/ChamID.pci, which CHAM.sys reads itself
		bool setFirmwareRoot(const std::filesystem::path& _root, std::string* _error = nullptr);

		// IRP_MJ_CREATE: boots ChamID.pci and reads the card identity
		bool open();
		Result ioctl(uint32_t _code, const std::vector<uint8_t>& _in, uint32_t _outSize);

		// Lets the card firmware run while the host is quiet
		void idle(uint64_t _dspInstructions);

		// Optional lock taken around each slice of the control processors (they write the Lexichips the
		// audio thread is running)
		void setCardLock(std::mutex* _lock) { m_lock = _lock; }

		bool cardReady() const { return m_ready; }
		std::function<void(const std::string&)> onLog;

		struct Stats { uint64_t commands = 0, completions = 0, timeouts = 0, messages = 0; };
		const Stats& stats() const { return m_stats; }

	private:
		// queue offsets in the common buffer
		static constexpr uint32_t QHost = 0x000, RHost = 0x248, QTmix = 0x288, Status = 0x4D0, QZ80a = 0x590, QZ80b = 0x7D8,
		                          RZ80a = 0xA20, RZ80b = 0xA60;

		uint32_t& cb(uint32_t _byteOffset) { return m_cb[(_byteOffset >> 2) & (CommonBufferSize / 4 - 1)]; }
		void step(uint64_t _dspInstructions);
		void serviceInterrupt();
		void dpc(uint32_t _w0, uint32_t _slot);
		template<typename F> bool runUntil(F _done, uint64_t _budget);
		uint32_t post(uint32_t _queue, uint32_t _vector, const std::array<uint32_t, 16>& _cmd);
		void advance(uint32_t _queue, uint32_t _vector);
		bool queueEmpty(uint32_t _queue) { return cb(_queue + 0x40) == cb(_queue + 0x44); }
		void readback(uint32_t _resp, bool _bytes);
		void log(const std::string& _s) { if (onLog) onLog(_s); }

		ReverbCard& m_card;
		Dsp56kBootImage m_chamId;
		std::array<uint32_t, CommonBufferSize / 4> m_cb{};
		struct CardConfig { uint32_t valid = 0, subvendor = 0, type = 0, dbver = 0, idr = 0; } m_cfg;
		bool m_ready = false;          // card+0: last HF1/HF2 message carried $A98765
		bool m_ackPending = false;     // HCVR $7B sent, waiting for the DSP to drop HINT
		uint64_t m_ackCount = 0;
		bool m_statusBusy = false;     // card+4
		uint32_t m_nextSlot = 1;
		Result m_async;                // events/readbacks produced since the last call
		Stats m_stats;
		std::mutex* m_lock = nullptr;
	};
}
