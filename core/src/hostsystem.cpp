#include "l960/hostsystem.h"

#include "l960/larc2/larc2device.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern const unsigned char kL960HookDll[];
extern const std::size_t kL960HookDllSize;

namespace l960
{
	namespace fs = std::filesystem;

	namespace
	{
		// Debugging: L960_LINK_LOG=<file> logs every LARC2 link frame (unescaped, between F0 and F7)
		struct LinkLog
		{
			FILE* f = nullptr;
			std::mutex lock;
			struct Dir { std::vector<uint8_t> buf; bool esc = false; } d[2];
			LinkLog() { if (const char* p = std::getenv("L960_LINK_LOG")) f = std::fopen(p, "w"); }
			void feed(int _dir, uint8_t _b)
			{
				if (!f) return;
				std::lock_guard<std::mutex> l(lock);
				auto& x = d[_dir];
				if (_b == 0xF0) { x.buf.clear(); x.esc = false; return; }
				if (_b == 0xF7)
				{
					std::fprintf(f, "%lld %s", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
					             std::chrono::steady_clock::now().time_since_epoch()).count() % 10000000, _dir ? "H>L" : "L>H");
					for (auto c : x.buf) std::fprintf(f, " %02x", c);
					std::fprintf(f, "\n");
					std::fflush(f);
					x.buf.clear();
					return;
				}
				if (_b == 0xFF) { x.esc = true; return; }
				x.buf.push_back(x.esc ? uint8_t(_b ^ 0x80) : _b);
				x.esc = false;
			}
		};
		LinkLog& linkLog() { static LinkLog l; return l; }
	}

	struct HostSystem::Helper
	{
#ifdef _WIN32
		PROCESS_INFORMATION pi{};
		HANDLE job = nullptr;  // kills the helper if the plugin's process goes away
		std::string drive;     // "X:" mapped to the data folder (the 960L's writable drive)
		std::string sysDrive;  // "Y:" whose \960L is the software copy (%SystemDrive% for the host software)
#else
		bool started = false;
#endif
	};

	HostSystem::HostSystem(ReverbCard& _card, std::mutex& _cardLock, larc2::Larc2Device& _larc2)
		: m_card(_card), m_cardLock(_cardLock), m_larc2(_larc2)
	{
	}

	HostSystem::~HostSystem()
	{
		stopHelper();
		stop();
	}

	bool HostSystem::start(const fs::path& _root, std::string* _error)
	{
		if (m_bridge) return true;

		m_cham = std::make_unique<ChamDevice>(m_card);
		m_cham->setCardLock(&m_cardLock);
		m_cham->onLog = [this](const std::string& s) { log("[card] " + s); };
		if (!m_cham->setFirmwareRoot(_root, _error)) { m_cham.reset(); return false; }

		std::string err;
		for (int port = 7960; port < 8060; port += 10)
		{
			auto b = std::make_unique<HostBridge>(port);
			b->onLog = [this](const std::string& s) { log("[bridge] " + s); };
			b->onOpen = [this] {
				std::lock_guard<std::mutex> l(m_chamLock);
				return m_cham->open();
			};
			b->onIoctl = [this](const HostBridge::Ioctl& _q) {
				std::lock_guard<std::mutex> l(m_chamLock);
				const auto res = m_cham->ioctl(_q.code, _q.in, _q.outSize);
				HostBridge::IoctlReply r;
				r.ok = res.ok;
				r.info = res.info;
				r.out = res.out;
				r.events = res.events;
				for (const auto& rb : res.readbacks) r.readbacks.push_back({rb.mdl, rb.bytes});
				return r;
			};
			b->onLarc2Data = [this](const uint8_t* _d, size_t _n) {
				for (size_t i = 0; i < _n; ++i) linkLog().feed(1, _d[i]);
				m_larc2.hostInput(_d, _n);
			};
			if (b->start(&err))
			{
				m_bridge = std::move(b);
				m_port = port;
				break;
			}
		}
		if (!m_bridge)
		{
			if (_error) *_error = "no free TCP port for the host bridge: " + err;
			m_cham.reset();
			return false;
		}
		HostBridge* bridge = m_bridge.get();
		m_larc2.setHostOutput([bridge](uint8_t _b) { linkLog().feed(0, _b); bridge->sendToHost(&_b, 1); });

		// Let the card firmware tick over between host requests (status, coefficient slewing)
		m_quit = false;
		m_idle = std::thread([this] {
			while (!m_quit)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				std::lock_guard<std::mutex> l(m_chamLock);
				if (m_cham->cardReady()) m_cham->idle(20000);
			}
		});
		log("host bridge listening on 127.0.0.1:" + std::to_string(m_port) + ".." + std::to_string(m_port + 2));
		return true;
	}

	void HostSystem::stop()
	{
		m_quit = true;
		if (m_idle.joinable()) m_idle.join();
		m_larc2.setHostOutput(nullptr);
		if (m_bridge) m_bridge->stop();
		m_bridge.reset();
		m_cham.reset();
		m_port = 0;
	}

	std::string HostSystem::status() const
	{
		if (!m_bridge) return "host bridge stopped";
		std::string s = "bridge :" + std::to_string(m_port);
		s += m_bridge->hostConnected() ? ", host connected" : ", waiting for the host software";
		if (m_cham)
		{
			const auto& st = m_cham->stats();
			s += ", card " + std::string(m_cham->cardReady() ? "running" : "idle") + ", " + std::to_string(st.commands) + " commands";
			if (st.timeouts) s += ", " + std::to_string(st.timeouts) + " timeouts";
		}
		return s;
	}

	// ------------------------------------------------------------------ helper process

	namespace
	{
		bool prepareCopy(const fs::path& _root, const fs::path& _copy, std::string* _error)
		{
			std::error_code ec;
			const auto exe = _copy / "App" / "Framework.exe";
			const auto srcExe = _root / "App" / "Framework.exe";
			if (!fs::exists(srcExe, ec)) { if (_error) *_error = "Framework.exe not found in " + (_root / "App").string(); return false; }
			if (!fs::exists(exe, ec) || fs::file_size(exe, ec) != fs::file_size(srcExe, ec))
			{
				fs::create_directories(_copy, ec);
				fs::copy(_root, _copy, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
				if (ec) { if (_error) *_error = "copying the 960L software failed: " + ec.message(); return false; }
			}
			// Install the hook: the original Enums.dll becomes EnumsOrig.dll, ours forwards to it
			const auto app = _copy / "App";
			if (!fs::exists(app / "EnumsOrig.dll", ec))
			{
				fs::copy_file(_root / "App" / "Enums.dll", app / "EnumsOrig.dll", fs::copy_options::overwrite_existing, ec);
				if (ec) { if (_error) *_error = "cannot install EnumsOrig.dll: " + ec.message(); return false; }
			}
			std::ofstream f(app / "Enums.dll", std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(kL960HookDll), static_cast<std::streamsize>(kL960HookDllSize));
			if (!f) { if (_error) *_error = "cannot write the hook DLL"; return false; }
			return true;
		}
	}

	bool HostSystem::launchHelper(const fs::path& _root, const fs::path& _dataDir, std::string* _error)
	{
		if (!m_bridge) { if (_error) *_error = "host bridge not running"; return false; }
		if (helperRunning()) return true;
		stopHelper();
		std::error_code ec;
		fs::create_directories(_dataDir, ec);
		const auto copy = _dataDir / "software" / "960L";
		if (!prepareCopy(_root, copy, _error)) return false;
		const auto drive = _dataDir / "drive";
		fs::create_directories(drive, ec);

		const std::string ports[3] = {std::to_string(m_port), std::to_string(m_port + 1), std::to_string(m_port + 2)};
		m_helper = std::make_unique<Helper>();

#ifdef _WIN32
		// Two drive letters for the helper (the host software only works with drive letters here):
		//  - the 960L's writable storage ("F:" on the hardware) -> <data>/drive
		//  - its system drive: paths like %SystemDrive%\960L\Larc2\larc2.rom (LARC2 version check) and
		//    %SystemDrive%\960L\App\Algorithms, so a letter whose \960L is our copy; the hook expands
		//    %SystemDrive% to it (L960_SYSTEM_DRIVE). Nothing changes for the plugin's own process.
		const DWORD used = GetLogicalDrives();
		auto mapFree = [&](const fs::path& _target, std::string& _out) {
			for (char c = 'Z'; c >= 'G'; --c)
			{
				if ((used & (1u << (c - 'A'))) || (!m_helper->drive.empty() && m_helper->drive[0] == c)) continue;
				const std::string d = std::string(1, c) + ":";
				if (DefineDosDeviceA(0, d.c_str(), _target.string().c_str())) { _out = d; return true; }
			}
			return false;
		};
		if (!mapFree(drive, m_helper->drive)) { if (_error) *_error = "no free drive letter for the 960L data drive"; m_helper.reset(); return false; }
		if (!mapFree(copy.parent_path(), m_helper->sysDrive))
		{
			if (_error) *_error = "no free drive letter for the 960L system drive";
			DefineDosDeviceA(DDD_REMOVE_DEFINITION, m_helper->drive.c_str(), nullptr);
			m_helper.reset();
			return false;
		}

		// Environment block for the helper: ours plus the settings above
		std::vector<std::pair<std::string, std::string>> vars = {
			{"L960_SYSTEM_DRIVE", m_helper->sysDrive}, {"L960_DATA_DRIVE", m_helper->drive}, {"L960_DATA_DIR", _dataDir.string()},
			{"L960_CHAM_PORT", ports[0]}, {"L960_COM1_PORT", ports[1]}, {"L960_COM2_PORT", ports[2]}};
		std::string env;
		if (char* block = GetEnvironmentStringsA())
		{
			for (const char* p = block; *p; p += std::strlen(p) + 1)
			{
				const std::string kv = p;
				const auto eq = kv.find('=', 1);
				const std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
				bool overridden = false;
				for (const auto& [k, v] : vars) if (_stricmp(k.c_str(), key.c_str()) == 0) overridden = true;
				if (!overridden) { env += kv; env.push_back('\0'); }
			}
			FreeEnvironmentStringsA(block);
		}
		for (const auto& [k, v] : vars) { env += k + "=" + v; env.push_back('\0'); }
		env.push_back('\0');

		const auto exe = (copy / "App" / "Framework.exe").string();
		const auto cwd = (copy / "App").string();
		STARTUPINFOA si{};
		si.cb = sizeof si;
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		std::string cmd = "\"" + exe + "\"";
		if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, env.data(), cwd.c_str(), &si, &m_helper->pi))
		{
			if (_error) *_error = "cannot start Framework.exe (error " + std::to_string(GetLastError()) + ")";
			DefineDosDeviceA(DDD_REMOVE_DEFINITION, m_helper->drive.c_str(), nullptr);
			DefineDosDeviceA(DDD_REMOVE_DEFINITION, m_helper->sysDrive.c_str(), nullptr);
			m_helper.reset();
			return false;
		}
		m_helper->job = CreateJobObjectA(nullptr, nullptr);
		if (m_helper->job)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(m_helper->job, JobObjectExtendedLimitInformation, &li, sizeof li);
			AssignProcessToJobObject(m_helper->job, m_helper->pi.hProcess);
		}
		log("started the 960L host software (system drive " + m_helper->sysDrive + ", data drive " + m_helper->drive + ")");
		return true;
#else
		const char* cmd = std::getenv("L960_HELPER_CMD");
		if (!cmd || !*cmd)
		{
			if (_error) *_error = "the 960L host software runs on Windows; set L960_HELPER_CMD to start it another way (e.g. with wine)";
			m_helper.reset();
			return false;
		}
		// The command gets the same settings through the environment
		setenv("L960_DATA_DIR", _dataDir.string().c_str(), 1);
		setenv("L960_CHAM_PORT", ports[0].c_str(), 1);
		setenv("L960_COM1_PORT", ports[1].c_str(), 1);
		setenv("L960_COM2_PORT", ports[2].c_str(), 1);
		setenv("L960_SOFTWARE_DIR", copy.string().c_str(), 1);
		setenv("L960_SYSTEM_ROOT", copy.parent_path().string().c_str(), 1);  // map a drive letter here, pass it as L960_SYSTEM_DRIVE
		const std::string line = std::string(cmd) + " &";
		m_helper->started = std::system(line.c_str()) == 0;
		return m_helper->started;
#endif
	}

	bool HostSystem::helperRunning() const
	{
		if (!m_helper) return false;
#ifdef _WIN32
		return m_helper->pi.hProcess && WaitForSingleObject(m_helper->pi.hProcess, 0) == WAIT_TIMEOUT;
#else
		return m_helper->started;
#endif
	}

	void HostSystem::stopHelper()
	{
		if (!m_helper) return;
#ifdef _WIN32
		if (m_helper->pi.hProcess)
		{
			TerminateProcess(m_helper->pi.hProcess, 0);
			WaitForSingleObject(m_helper->pi.hProcess, 3000);
			CloseHandle(m_helper->pi.hProcess);
			CloseHandle(m_helper->pi.hThread);
		}
		if (m_helper->job) CloseHandle(m_helper->job);
		if (!m_helper->drive.empty()) DefineDosDeviceA(DDD_REMOVE_DEFINITION, m_helper->drive.c_str(), nullptr);
		if (!m_helper->sysDrive.empty()) DefineDosDeviceA(DDD_REMOVE_DEFINITION, m_helper->sysDrive.c_str(), nullptr);
#endif
		m_helper.reset();
	}
}
