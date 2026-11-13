#include "l960/larc2/larc2device.h"
#include "l960/firmware.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace l960::larc2
{
	const char* const Larc2Device::KeyNames[5][8] = {
		{"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"},
		{"1", "2", "3", "PROGRAM", "REGISTER", "", "Up", "JOYSTICK"},
		{"4", "5", "6", "BANK", "STORE", "Left", "", "Right"},
		{"7", "8", "9", "EDIT", "CONTROL", "", "Down", "FINE ADJUST"},
		{"MUTE MACH", "0", "MUTE ALL", "MACHINE", "ENTER", "-", "+", "LEXICON"},
	};

	namespace
	{
		bool special(uint8_t _b) { return _b == 0xF0 || _b == 0xF7 || _b == 0xF9 || _b == 0xFB || _b == 0xFF; }
		constexpr double CpuHz = 206.4e6;
	}

	// ------------------------------------------------------------------ codec

	bool HostLinkCodec::feed(const uint8_t _b, HostFrame& _out)
	{
		if (_b == 0xF0) { m_buf.clear(); m_inFrame = true; m_escape = false; return false; }
		if (!m_inFrame) return false;
		if (_b == 0xF7)
		{
			m_inFrame = false;
			// m_buf: 05 cmd payload... csumLo csumHi
			if (m_buf.size() < 4 || m_buf[0] != 0x05) return false;
			uint16_t sum = 0;
			for (size_t i = 0; i + 2 < m_buf.size(); ++i) sum = uint16_t(sum + m_buf[i]);
			const uint16_t got = uint16_t(m_buf[m_buf.size() - 2] | (m_buf[m_buf.size() - 1] << 8));
			if (sum != got) return false;
			_out.cmd = m_buf[1];
			_out.payload.assign(m_buf.begin() + 2, m_buf.end() - 2);
			return true;
		}
		if (_b == 0xFF) { m_escape = true; return false; }
		m_buf.push_back(m_escape ? uint8_t(_b ^ 0x80) : _b);
		m_escape = false;
		return false;
	}

	std::vector<uint8_t> HostLinkCodec::encodeToLarc2(const uint16_t _type, const std::vector<uint8_t>& _payload, const uint8_t _addr)
	{
		std::vector<uint8_t> body = {_addr, uint8_t(_type), uint8_t(_type >> 8), uint8_t(_payload.size()), uint8_t(_payload.size() >> 8)};
		body.insert(body.end(), _payload.begin(), _payload.end());
		uint16_t sum = 5;
		for (uint8_t b : body) sum = uint16_t(sum + b);
		body.push_back(uint8_t(sum));
		body.push_back(uint8_t(sum >> 8));
		std::vector<uint8_t> out = {0xF0, 0x05};
		for (uint8_t b : body)
		{
			if (special(b)) { out.push_back(0xFF); out.push_back(uint8_t(b ^ 0x80)); }
			else out.push_back(b);
		}
		out.push_back(0xF7);
		return out;
	}

	// ------------------------------------------------------------------ device

	Larc2Device::Larc2Device()
	{
		m_sys.onSerialOutput = [this](int port, uint8_t b) {
			if (port == 1) onHostByte(b);
			else if (port == 3)
			{
				if (b == '\n') { if (m_console.size() > 4000) m_console.erase(0, 2000); m_console.push_back('\n'); }
				else if (b >= 32 && b < 127) m_console.push_back(char(b));
			}
		};
	}

	Larc2Device::~Larc2Device() { stop(); }

	bool Larc2Device::load(const std::filesystem::path& _root, std::string* _error)
	{
		stop();
		std::vector<uint8_t> boot, app;
		const auto dir = std::filesystem::exists(_root / "960L") ? _root / "960L" / "Larc2" : _root / "Larc2";
		if (!readFile(dir / "boot.rom", boot) || !readFile(dir / "larc2.rom", app))
		{
			if (_error) *_error = "missing Larc2/boot.rom or Larc2/larc2.rom";
			return false;
		}
		std::lock_guard<std::mutex> l(m_lock);
		m_sys.loadBootRom(boot);
		m_sys.loadFlash(app, 0x180000);  // where the boot ROM's "Update LARC2 Software" saves it (boot.rom 0x133A4)
		m_sys.reset();
		m_codec = {};
		m_linked = false;
		m_state = "loaded";
		return true;
	}

	void Larc2Device::hostInput(const uint8_t* _data, const size_t _n)
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (m_linked && _n) m_state = "application running, connected to the 960L host software";
		// Delivered at the link's 57600 baud by the emulation thread: the host sends whole screens in one go,
		// and handing them to the UART at once overran the CE serial driver (lost screen objects)
		m_hostRx.insert(m_hostRx.end(), _data, _data + _n);
	}

	void Larc2Device::setHostOutput(std::function<void(uint8_t)> _f)
	{
		std::lock_guard<std::mutex> l(m_lock);
		onHostOutput = std::move(_f);
	}

	void Larc2Device::onHostByte(const uint8_t _b)
	{
		if (m_linked && onHostOutput) { onHostOutput(_b); }
		HostFrame f;
		if (!m_codec.feed(_b, f)) return;
		m_lastHostCmd = f.cmd;
		auto send = [this](uint16_t type) {
			for (uint8_t b : HostLinkCodec::encodeToLarc2(type, {})) m_sys.serialInput(1, b);
		};
		switch (f.cmd)
		{
		case 6: m_state = "boot ROM found host"; send(9); break;
		case 8: m_state = "booting application"; send(7); m_linked = true; break;
		case 10: if (!onHostOutput) m_state = "application waiting for the 960L host software"; break;
		default: break;
		}
	}

	void Larc2Device::start()
	{
		if (m_thread.joinable()) return;
		m_quit = false;
		m_thread = std::thread([this] { threadFunc(); });
	}

	void Larc2Device::stop()
	{
		m_quit = true;
		if (m_thread.joinable()) m_thread.join();
	}

	void Larc2Device::threadFunc()
	{
		using clock = std::chrono::steady_clock;
		auto last = clock::now();
		auto lastFrame = last;
		double budget = 0;
		while (!m_quit)
		{
			const auto now = clock::now();
			budget += std::chrono::duration<double>(now - last).count() * CpuHz;
			last = now;
			budget = std::min(budget, CpuHz * 0.05);  // never try to catch up more than 50 ms
			const auto n = static_cast<uint64_t>(std::min(budget, 2.0e6));
			if (n >= 4096)
			{
				std::lock_guard<std::mutex> l(m_lock);
				// one byte per character time of the host link (57600 baud, 10 bits) in emulated CPU time
				constexpr uint64_t InstrPerByte = static_cast<uint64_t>(CpuHz / 5760.0);
				uint64_t left = n;
				while (left)
				{
					if (!m_hostRx.empty() && m_rxCredit >= InstrPerByte)
					{
						m_sys.serialInput(1, m_hostRx.front());
						m_hostRx.pop_front();
						m_rxCredit -= InstrPerByte;
					}
					const uint64_t step = std::min<uint64_t>(left, InstrPerByte);
					m_sys.run(step);
					left -= step;
					m_rxCredit = m_hostRx.empty() ? InstrPerByte : std::min<uint64_t>(m_rxCredit + step, 4 * InstrPerByte);
				}
				m_instr += n;
				budget -= double(n);
			}
			else std::this_thread::sleep_for(std::chrono::milliseconds(1));

			if (now - lastFrame > std::chrono::milliseconds(33))
			{
				lastFrame = now;
				std::lock_guard<std::mutex> l(m_lock);
				m_sys.renderLcd(m_frame, m_fw, m_fh);
				++m_frameNo;
			}
		}
	}

	bool Larc2Device::pressKey(const std::string& _name, const bool _down)
	{
		for (int r = 0; r < 5; ++r)
			for (int c = 0; c < 8; ++c)
				if (_name == KeyNames[r][c])
				{
					std::lock_guard<std::mutex> l(m_lock);
					m_sys.setKey(r, c, _down);
					return true;
				}
		return false;
	}

	void Larc2Device::setFader(const int _i, const float _pos01)
	{
		std::lock_guard<std::mutex> l(m_lock);
		m_sys.setFader(_i, uint16_t(std::clamp(_pos01, 0.0f, 1.0f) * 1023.0f));
	}

	void Larc2Device::setFaderTouched(const int _i, const bool _t)
	{
		std::lock_guard<std::mutex> l(m_lock);
		m_sys.setFaderTouched(_i, _t);
	}

	void Larc2Device::setJoystick(const float _x01, const float _y01)
	{
		std::lock_guard<std::mutex> l(m_lock);
		m_sys.setJoystick(uint16_t(std::clamp(_x01, 0.0f, 1.0f) * 1023.0f), uint16_t(std::clamp(_y01, 0.0f, 1.0f) * 1023.0f));
	}

	uint64_t Larc2Device::lcd(std::vector<uint8_t>& _rgb, uint32_t& _w, uint32_t& _h)
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (m_frame.empty()) m_sys.renderLcd(m_frame, m_fw, m_fh);
		_rgb = m_frame;
		_w = m_fw;
		_h = m_fh;
		return m_frameNo;
	}

	std::string Larc2Device::fpgaTrace()
	{
		std::lock_guard<std::mutex> l(m_lock);
		std::string out;
		char b[96];
		for (const auto& [k, n] : m_sys.fpgaTrace)
		{
			if (k & 0x80000000u) std::snprintf(b, sizeof b, "W %03X <- %03X : %llu\n", (k >> 12) & 0x7FFFF, k & 0xFFF, (unsigned long long)n);
			else std::snprintf(b, sizeof b, "R %03X : %llu\n", k, (unsigned long long)n);
			out += b;
		}
		m_sys.fpgaTrace.clear();
		return out;
	}

	std::string Larc2Device::status() const
	{
		std::lock_guard<std::mutex> l(m_lock);
		return m_state;
	}
}
