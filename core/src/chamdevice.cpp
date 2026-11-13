#include "l960/chamdevice.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace l960
{
	namespace
	{
		constexpr uint32_t ReadyWord = 0xA98765;
		constexpr uint64_t SliceDsp = 2000;  // 56301 instructions per slice (~20 us at 100 MHz)
		// Z80 T-states per slice. The real clock ratio is unknown; the host's start-up SRAM test (ChamAPI BootDiags,
		// 16 x 16 KB fill/verify per Z80) must finish well inside LICS's 45 s unit start-up timeout, so the Z80s
		// are given as much time as the 56301 here.
		constexpr uint32_t SliceZ80 = 2000;

		// ERRVALs that CHAM.sys hands back in IRP Information
		constexpr uint32_t ErrQueueFull = 0x10003, ErrDownload56K = 0x10008, ErrConfigSize = 0x1000A, ErrDownloadZ80 = 0x1000B,
		                   ErrInitQueues = 0x1000C;

		uint32_t rd32(const std::vector<uint8_t>& _b, size_t _o)
		{
			if (_o + 4 > _b.size()) return 0;
			return uint32_t(_b[_o]) | uint32_t(_b[_o + 1]) << 8 | uint32_t(_b[_o + 2]) << 16 | uint32_t(_b[_o + 3]) << 24;
		}

		void put32(std::vector<uint8_t>& _b, size_t _o, uint32_t _v)
		{
			for (int i = 0; i < 4; ++i) _b[_o + i] = uint8_t(_v >> (8 * i));
		}

		std::string hexs(uint32_t _v)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%X", _v);
			return b;
		}
	}

	ChamDevice::ChamDevice(ReverbCard& _card) : m_card(_card)
	{
		auto& dsp = m_card.dsp();
		dsp.pciRead = [this](uint32_t _a) -> uint32_t {
			if (_a - CommonBufferPhys < CommonBufferSize) return cb(_a - CommonBufferPhys);
			log("PCI master read outside the common buffer: $" + hexs(_a));
			return 0;
		};
		dsp.pciWrite = [this](uint32_t _a, uint32_t _v) {
			if (_a - CommonBufferPhys < CommonBufferSize) cb(_a - CommonBufferPhys) = _v;
			else log("PCI master write outside the common buffer: $" + hexs(_a));
		};
	}

	bool ChamDevice::setFirmwareRoot(const std::filesystem::path& _root, std::string* _error)
	{
		std::vector<uint8_t> f;
		auto p = _root / "Cham" / "ChamID.pci";
		if (!readFile(p, f)) p = _root / "960L" / "Cham" / "ChamID.pci";
		if (!readFile(p, f))
		{
			if (_error) *_error = "cannot read Cham/ChamID.pci";
			return false;
		}
		return m_chamId.parse(f, _error);
	}

	void ChamDevice::step(const uint64_t _n)
	{
		if (m_lock)
		{
			std::lock_guard<std::mutex> l(*m_lock);
			m_card.stepControl(uint32_t(_n * SliceZ80 / SliceDsp), _n);
		}
		else
			m_card.stepControl(uint32_t(_n * SliceZ80 / SliceDsp), _n);
		// Word clock as measured by the 56301 timers (LICS: fs = 40 MHz / period; 0 = "Lost wordclock")
		const uint32_t fs = m_card.io().sampleRate() ? m_card.io().sampleRate() : 48000;
		m_card.dsp().timerCapture(40000000 / fs, 40000000 / fs);
		serviceInterrupt();
	}

	template<typename F> bool ChamDevice::runUntil(F _done, const uint64_t _budget)
	{
		const auto t0 = std::chrono::steady_clock::now();
		uint32_t k = 0;
		for (uint64_t n = 0; n < _budget; n += SliceDsp)
		{
			if (_done()) return true;
			step(SliceDsp);
			// wall-clock guard: the host waits at most a few seconds for the card anyway
			if ((++k & 255) == 0)
			{
				static const bool trace = std::getenv("L960_CHAM_TRACE") != nullptr;
				if (trace) log("  run: PC $" + hexs(m_card.dsp().pc()) + ", " + std::to_string(m_card.dsp().instructions()) + " instructions");
				if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(4)) break;
			}
		}
		if (_done()) return true;
		++m_stats.timeouts;
		return false;
	}

	void ChamDevice::idle(const uint64_t _n)
	{
		for (uint64_t i = 0; i < _n; i += SliceDsp) step(SliceDsp);
	}

	// ---------------------------------------------------------------- ISR / DPC (CHAM.sys 0x11457 / 0x11560)

	void ChamDevice::serviceInterrupt()
	{
		auto& dsp = m_card.dsp();
		// the DSP drops HINT in its $7A handler and may raise it again for the next message right away
		if (m_ackPending)
		{
			if (dsp.hostInterruptAcks() == m_ackCount) return;
			m_ackPending = false;
		}
		if (!dsp.hostInterrupt()) return;

		const uint32_t f = (dsp.hostRead(Dsp56301::HSTR) >> 3) & 7;
		m_ackCount = dsp.hostInterruptAcks();
		dsp.hostWrite(Dsp56301::HCVR, 0x7B);
		m_ackPending = true;
		++m_stats.messages;
		if (f == 1 || f == 2)
		{
			const uint32_t w = dsp.hostRead(Dsp56301::HTXR_HRXS);
			m_ready = w == ReadyWord;
			log(std::string("card message HF") + char('0' + f) + " $" + hexs(w));
		}
		else if (f == 3)
		{
			const uint32_t w0 = dsp.hostRead(Dsp56301::HTXR_HRXS);
			const uint32_t w1 = dsp.hostRead(Dsp56301::HTXR_HRXS);
			const uint32_t w2 = dsp.hostRead(Dsp56301::HTXR_HRXS);
			dpc(w0, (w2 << 16) | (w1 & 0xFFFF));
		}
		else
			log("card interrupt with HF" + std::to_string(f));
	}

	void ChamDevice::readback(const uint32_t _resp, const bool _bytes)
	{
		Readback r;
		r.mdl = (cb(_resp) & 0xFFFF) | (cb(_resp + 4) << 16);
		const uint32_t n = cb(_resp + 8);
		if (_bytes)
		{
			// Z80 response: n bytes, 3 per dword, most significant first
			for (uint32_t i = 0; i < n && i < 3 * 13; ++i)
			{
				const uint32_t w = cb(_resp + 0xC + 4 * (i / 3)) & 0xFFFFFF;
				r.bytes.push_back(uint8_t(w >> (16 - 8 * (i % 3))));
			}
		}
		else
		{
			for (uint32_t i = 0; i < n && i < 13; ++i)
			{
				const uint32_t w = cb(_resp + 0xC + 4 * i) & 0xFFFFFF;
				for (int k = 0; k < 4; ++k) r.bytes.push_back(uint8_t(w >> (8 * k)));
			}
		}
		m_async.readbacks.push_back(std::move(r));
	}

	void ChamDevice::dpc(const uint32_t _w0, const uint32_t _slot)
	{
		m_card.dsp().hostWrite(Dsp56301::HCVR, 0x87);
		const bool hasData = (_w0 & 0xFF0000) == 0x10000;
		if (_w0 & 0xFF00) log("card reports error " + std::to_string((_w0 >> 8) & 0xFF) + " for class " + std::to_string(_w0 & 0xFF));
		switch (_w0 & 0xFF)
		{
		case 1:
			if (hasData) readback(RHost, false);
			if (_slot) m_async.events.push_back(_slot);
			advance(QHost, 0x7D);
			break;
		case 2:
		case 3:
		{
			const bool b = (_w0 & 0xFF) == 3;
			if (hasData) readback(b ? RZ80b : RZ80a, true);
			if (_slot) m_async.events.push_back(_slot);
			advance(b ? QZ80b : QZ80a, b ? 0x85 : 0x83);
			break;
		}
		case 4:
			if (_slot) m_async.events.push_back(_slot);
			advance(QTmix, 0x7F);
			break;
		case 5:
			cb(Status) = (_w0 & 0xFF00) ? 2 : 0;
			m_statusBusy = false;
			return;
		default:
			log("unknown card message class $" + hexs(_w0));
			return;
		}
		++m_stats.completions;
	}

	// ---------------------------------------------------------------- queues

	uint32_t ChamDevice::post(const uint32_t _q, const uint32_t _vector, const std::array<uint32_t, 16>& _cmd)
	{
		const uint32_t rd = cb(_q + 0x40) & 7, wr = cb(_q + 0x44) & 7;
		if (((wr + 1) & 7) == rd) return ErrQueueFull;
		for (uint32_t i = 0; i < 16; ++i) cb(_q + 0x48 + 64 * wr + 4 * i) = _cmd[i];
		if (wr == rd)
		{
			for (uint32_t i = 0; i < 16; ++i) cb(_q + 4 * i) = _cmd[i];
			m_card.dsp().hostWrite(Dsp56301::HCVR, _vector);
		}
		cb(_q + 0x44) = (wr + 1) & 7;
		++m_stats.commands;
		return 0;
	}

	void ChamDevice::advance(const uint32_t _q, const uint32_t _vector)
	{
		const uint32_t rd = (cb(_q + 0x40) + 1) & 7, wr = cb(_q + 0x44) & 7;
		cb(_q + 0x40) = rd;
		if (rd == wr) return;
		for (uint32_t i = 0; i < 16; ++i) cb(_q + 4 * i) = cb(_q + 0x48 + 64 * rd + 4 * i);
		m_card.dsp().hostWrite(Dsp56301::HCVR, _vector);
	}

	// ---------------------------------------------------------------- open (IRP_MJ_CREATE)

	bool ChamDevice::open()
	{
		m_cfg = {};
		m_ready = false;
		m_ackPending = false;
		m_statusBusy = false;
		m_cb.fill(0);
		if (m_chamId.words.empty())
		{
			log("ChamID.pci not loaded");
			return false;
		}

		{
			std::unique_lock<std::mutex> l;
			if (m_lock) l = std::unique_lock<std::mutex>(*m_lock);
			m_card.reset();
		}
		auto& dsp = m_card.dsp();
		dsp.hostWrite(Dsp56301::HCTR, 0xA00);
		dsp.bootstrap(m_chamId);
		dsp.hostWrite(Dsp56301::HCVR, 0x79);
		dsp.hostWrite(Dsp56301::HTXR_HRXS, CommonBufferPhys & 0xFFFF);
		dsp.hostWrite(Dsp56301::HTXR_HRXS, CommonBufferPhys >> 16);
		if (!runUntil([&] { return cb(0) != 0; }, 300'000'000))
		{
			log("ChamID did not answer (PC $" + hexs(dsp.pc()) + ")");
			return false;
		}
		m_cfg.valid = 1;
		m_cfg.type = (cb(4) >> 6) & 3;
		m_cfg.dbver = (cb(8) >> 4) & 3;
		m_cfg.idr = cb(12);
		log("ChamID: type " + std::to_string(m_cfg.type) + ", DB version " + std::to_string(m_cfg.dbver) + ", IDR $" + hexs(m_cfg.idr));
		m_cb.fill(0);
		return true;
	}

	// ---------------------------------------------------------------- IOCTLs

	ChamDevice::Result ChamDevice::ioctl(const uint32_t _code, const std::vector<uint8_t>& _in, const uint32_t _outSize)
	{
		Result r;
		auto& dsp = m_card.dsp();

		auto command = [&](uint32_t _queue, uint32_t _vector) {
			if (_in.size() < 64) { r.ok = false; return; }
			std::array<uint32_t, 16> c{};
			for (size_t i = 0; i < 16; ++i) c[i] = rd32(_in, 4 * i);
			auto split = [&](size_t lo) {
				const uint32_t v = (c[lo + 1] << 16) | c[lo];
				c[lo] = v & 0xFFFF;
				c[lo + 1] = v >> 16;
			};
			if ((c[4] << 16 | c[3]) != 0) split(3);  // event slot
			if (_code == HostCommand && c[0] == 5) split(8);
			if (_code == HostCommand && c[0] == 6) split(7);
			if (_code == Z80Command && c[0] == 2) split(8);
			r.info = post(_queue, _vector, c);
			if (r.info) return;
			if (!runUntil([&] { return queueEmpty(_queue); }, 400'000'000))
				log("command timed out: ioctl $" + hexs(_code) + " op " + std::to_string(c[0]) + " (PC $" + hexs(dsp.pc()) + ")");
		};

		switch (_code)
		{
		case GetConfiguration:
		{
			if (_outSize != 0x2C) { r.info = ErrConfigSize; break; }
			r.out.assign(0x2C, 0);
			r.out[0] = 0x6A;
			r.out[2] = m_cfg.valid ? 1 : 0;
			if (m_cfg.valid)
			{
				put32(r.out, 4, m_cfg.valid);
				put32(r.out, 8, m_cfg.subvendor);
				put32(r.out, 12, m_cfg.type);
				put32(r.out, 16, m_cfg.dbver);
				put32(r.out, 20, m_cfg.idr);
			}
			r.info = 0x2C;
			break;
		}
		case Download56KCode:
		{
			std::vector<uint8_t> file(_in.begin() + std::min<size_t>(4, _in.size()), _in.end());
			Dsp56kBootImage img;
			std::string err;
			if (!img.parse(file, &err)) { log("56K download: " + err); r.info = ErrDownload56K; break; }
			m_ready = false;
			dsp.hostWrite(Dsp56301::HCTR, 0xA00);
			dsp.bootstrap(img);
			log("56K code: " + std::to_string(img.words.size()) + " words at P:$" + hexs(img.loadAddress));
			runUntil([&] { return m_ready; }, 50'000'000);
			break;
		}
		case DownloadZ80Kernel:
		{
			if (_in.size() < 6) { r.info = ErrDownloadZ80; break; }
			m_ready = false;
			dsp.hostWrite(Dsp56301::HCTR, 0xA00);
			dsp.hostWrite(Dsp56301::HCVR, 0x75);
			const uint32_t n = uint32_t(_in.size() - 6);
			dsp.hostWrite(Dsp56301::HTXR_HRXS, n);
			dsp.hostWrite(Dsp56301::HTXR_HRXS, 0);
			for (uint32_t i = 0; i < n; ++i) dsp.hostWrite(Dsp56301::HTXR_HRXS, _in[4 + i]);
			if (!runUntil([&] { return m_ready; }, 400'000'000))
				log("Z80 kernel: card did not report ready (PC $" + hexs(dsp.pc()) + ", " + std::to_string(dsp.hostTxPending()) + " words unread)");
			break;
		}
		case InitializeQueues:
		{
			for (uint32_t q : {QHost, QTmix, QZ80a, QZ80b}) cb(q + 0x40) = cb(q + 0x44) = 0;
			m_ready = false;
			dsp.hostWrite(Dsp56301::HCVR, 0x79);
			for (uint32_t off : {QHost, RHost, QTmix, Status, QZ80a, QZ80b, RZ80a, RZ80b})
			{
				const uint32_t a = CommonBufferPhys + off;
				dsp.hostWrite(Dsp56301::HTXR_HRXS, a & 0xFFFF);
				dsp.hostWrite(Dsp56301::HTXR_HRXS, a >> 16);
			}
			if (!runUntil([&] { return m_ready; }, 100'000'000))
			{
				log("InitializeQueues: card did not report ready (PC $" + hexs(dsp.pc()) + ")");
				r.info = ErrInitQueues;
			}
			break;
		}
		case HostCommand: command(QHost, 0x7D); break;
		case TmixCommand:
			if (const char* p = std::getenv("L960_TMIX_LOG"))
			{
				static FILE* f = std::fopen(p, "w");
				if (f)
				{
					for (size_t i = 0; i + 3 < _in.size() && i < 64; i += 4) std::fprintf(f, "%X ", rd32(_in, i));
					std::fprintf(f, "\n");
					std::fflush(f);
				}
			}
			command(QTmix, 0x7F);
			break;
		case Z80Command:
		{
			const uint32_t unit = rd32(_in, 8);
			command(unit == 3 ? QZ80b : QZ80a, unit == 3 ? 0x85 : 0x83);
			break;
		}
		case GetPeriodicStatus:
		{
			if (m_statusBusy) { r.info = ErrQueueFull; break; }
			m_statusBusy = true;
			r.out.resize(0xC0);
			for (uint32_t i = 0; i < 0xC0 / 4; ++i) put32(r.out, 4 * i, cb(Status + 4 * i));
			dsp.hostWrite(Dsp56301::HCVR, 0x81);
			runUntil([&] { return !m_statusBusy; }, 100'000'000);
			break;
		}
		case RegisterEvent:
		case LockBuffer:
			// normally completed by the caller (they carry user-mode handles); hand out a slot anyway
			r.out.resize(4);
			put32(r.out, 0, m_nextSlot);
			m_nextSlot = m_nextSlot % 23 + 1;
			r.info = 4;
			break;
		default:
			r.ok = false;
			break;
		}

		r.events = std::move(m_async.events);
		r.readbacks = std::move(m_async.readbacks);
		m_async = {};
		return r;
	}
}
