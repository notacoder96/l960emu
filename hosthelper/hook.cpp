// l960 host hook: a proxy for Enums.dll that runs inside the original 960L host application (Framework.exe).
//
// Enums.dll is tiny (5 exports) and imported by every 960L host module, so a proxy of it is loaded before any
// of them runs. Its DllMain patches the import tables of all loaded modules (and of modules loaded later via
// LoadLibraryA) so that:
//   - the reverb-card driver (CHAM.sys, opened by ChamAPI.dll) becomes a virtual device whose IOCTLs are sent
//     to the emulator over TCP,
//   - COM1/COM2 (the LARC2 link opened by LightningInterface.dll) become TCP byte streams to the emulated LARC2,
//   - service-manager calls that install the driver succeed without doing anything.
// Everything is logged to C:\960L\l960hook.log.
//
// Build: i686-w64-mingw32-g++ (32-bit, like the original DLLs). The original Enums.dll must be renamed
// EnumsOrig.dll; enums.def forwards its exports.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
	// ------------------------------------------------------------------ logging

	CRITICAL_SECTION g_logLock;
	std::string g_logPath = "l960hook.log";
	FILE* g_log = nullptr;

	void logf(const char* _fmt, ...)
	{
		EnterCriticalSection(&g_logLock);
		if (!g_log) g_log = std::fopen(g_logPath.c_str(), "a");
		if (g_log)
		{
			std::fprintf(g_log, "[%08lu] ", GetTickCount());
			va_list a;
			va_start(a, _fmt);
			std::vfprintf(g_log, _fmt, a);
			va_end(a);
			std::fputc('\n', g_log);
			std::fflush(g_log);
		}
		LeaveCriticalSection(&g_logLock);
	}

	std::string hex(const void* _p, size_t _n, size_t _max = 64)
	{
		std::string s;
		char b[4];
		for (size_t i = 0; i < _n && i < _max; ++i) { std::snprintf(b, sizeof b, "%02x ", static_cast<const uint8_t*>(_p)[i]); s += b; }
		if (_n > _max) s += "...";
		return s;
	}

	// ------------------------------------------------------------------ TCP bridge

	std::string envOr(const char* _name, const char* _def)
	{
		char buf[256];
		const DWORD n = GetEnvironmentVariableA(_name, buf, sizeof buf);
		return n && n < sizeof buf ? std::string(buf, n) : std::string(_def);
	}

	SOCKET connectTo(int _port)
	{
		static bool wsa = false;
		if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) return s;
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u_short>(_port));
		a.sin_addr.s_addr = inet_addr(envOr("L960_BRIDGE_HOST", "127.0.0.1").c_str());
		if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
		BOOL nd = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
		return s;
	}

	bool sendAll(SOCKET _s, const void* _p, int _n)
	{
		const char* p = static_cast<const char*>(_p);
		while (_n > 0) { const int k = send(_s, p, _n, 0); if (k <= 0) return false; p += k; _n -= k; }
		return true;
	}

	bool recvAll(SOCKET _s, void* _p, int _n)
	{
		char* p = static_cast<char*>(_p);
		while (_n > 0) { const int k = recv(_s, p, _n, 0); if (k <= 0) return false; p += k; _n -= k; }
		return true;
	}

	// ------------------------------------------------------------------ virtual devices

	// Fake handles. Real kernel handles are multiples of 4 and far below these.
	const HANDLE HCham = reinterpret_cast<HANDLE>(0x7F000101);
	const HANDLE HCom1 = reinterpret_cast<HANDLE>(0x7F000201);
	const HANDLE HCom2 = reinterpret_cast<HANDLE>(0x7F000301);
	const SC_HANDLE HScm = reinterpret_cast<SC_HANDLE>(0x7F000401);
	const SC_HANDLE HSvc = reinterpret_cast<SC_HANDLE>(0x7F000501);

	bool isCom(HANDLE _h) { return _h == HCom1 || _h == HCom2; }

	struct ComPort
	{
		SOCKET sock = INVALID_SOCKET;
		COMMTIMEOUTS timeouts{};
		DCB dcb{};
		int port = 0;
	};
	ComPort g_com[2];
	CRITICAL_SECTION g_comLock[2];
	SOCKET g_cham = INVALID_SOCKET;
	CRITICAL_SECTION g_chamLock;

	ComPort& com(HANDLE _h) { return g_com[_h == HCom1 ? 0 : 1]; }
	CRITICAL_SECTION& comLock(HANDLE _h) { return g_comLock[_h == HCom1 ? 0 : 1]; }

	void completeOverlapped(LPOVERLAPPED _ov, DWORD _bytes)
	{
		if (!_ov) return;
		_ov->Internal = 0;
		_ov->InternalHigh = _bytes;
		if (_ov->hEvent) SetEvent(_ov->hEvent);
	}

	// ------------------------------------------------------------------ hooks

	using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
	using DeviceIoControl_t = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using WriteFile_t = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
	using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
	using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
	using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);

	CreateFileA_t realCreateFileA;
	DeviceIoControl_t realDeviceIoControl;
	ReadFile_t realReadFile;
	WriteFile_t realWriteFile;
	GetOverlappedResult_t realGetOverlappedResult;
	CloseHandle_t realCloseHandle;
	LoadLibraryA_t realLoadLibraryA;

	void patchModule(HMODULE _m);
	std::string dataDir();

	HANDLE WINAPI hkCreateFileA(LPCSTR _name, DWORD _acc, DWORD _share, LPSECURITY_ATTRIBUTES _sa, DWORD _disp, DWORD _flags, HANDLE _tmpl)
	{
		const std::string n = _name ? _name : "";
		std::string u = n;
		for (auto& c : u) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
		if (u.find("CHAM") != std::string::npos && u.rfind("\\\\.\\", 0) == 0)
		{
			// IRP_MJ_CREATE: the emulated driver boots ChamID.pci on the card; a failure fails CreateFile like CHAM.sys
			EnterCriticalSection(&g_chamLock);
			if (g_cham == INVALID_SOCKET) g_cham = connectTo(std::stoi(envOr("L960_CHAM_PORT", "7960")));
			uint32_t ok = 0;
			if (g_cham != INVALID_SOCKET)
			{
				const uint8_t tag = 'O';
				if (!sendAll(g_cham, &tag, 1) || !recvAll(g_cham, &ok, 4)) { closesocket(g_cham); g_cham = INVALID_SOCKET; ok = 0; }
			}
			LeaveCriticalSection(&g_chamLock);
			logf("CreateFileA(%s) -> virtual CHAM device (%s)", n.c_str(), g_cham == INVALID_SOCKET ? "bridge not running" : ok ? "card ready" : "card did not answer");
			if (!ok) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
			return HCham;
		}
		if (u == "COM1:" || u == "COM1" || u == "\\\\.\\COM1" || u == "COM2:" || u == "COM2" || u == "\\\\.\\COM2")
		{
			const bool one = u.find('1') != std::string::npos;
			const HANDLE h = one ? HCom1 : HCom2;
			auto& c = com(h);
			c.port = std::stoi(envOr(one ? "L960_COM1_PORT" : "L960_COM2_PORT", one ? "7961" : "7962"));
			EnterCriticalSection(&comLock(h));
			if (c.sock == INVALID_SOCKET) c.sock = connectTo(c.port);
			LeaveCriticalSection(&comLock(h));
			logf("CreateFileA(%s, flags %08lx) -> virtual serial port, TCP %d %s", n.c_str(), _flags, c.port, c.sock == INVALID_SOCKET ? "(not connected)" : "(connected)");
			return h;
		}
		// The host logs to C:\Thunder.log; keep it with the rest of the emulator's data (no admin rights needed)
		if (u == "C:\\THUNDER.LOG")
		{
			const std::string p = dataDir() + "\\Thunder.log";
			return realCreateFileA(p.c_str(), _acc, _share, _sa, _disp, _flags, _tmpl);
		}
		const HANDLE h = realCreateFileA(_name, _acc, _share, _sa, _disp, _flags, _tmpl);
		if (u.rfind("\\\\.\\", 0) == 0 || std::getenv("L960_HOOK_VERBOSE")) logf("CreateFileA(%s) passthrough -> %p", n.c_str(), h);
		return h;
	}

	// CHAM.sys keeps user-mode objects for later: event handles (IOCTL 0x60) it signals when the card
	// completes a command, and locked buffers (IOCTL 0x64) it fills with readback data. Those live here.
	HANDLE g_events[24] = {};
	struct Locked { void* va = nullptr; uint32_t len = 0; };
	Locked g_locked[32];

	BOOL WINAPI hkDeviceIoControl(HANDLE _h, DWORD _code, LPVOID _in, DWORD _inSize, LPVOID _out, DWORD _outSize, LPDWORD _ret, LPOVERLAPPED _ov)
	{
		if (_h != HCham) return realDeviceIoControl(_h, _code, _in, _inSize, _out, _outSize, _ret, _ov);
		const auto* in32 = static_cast<const uint32_t*>(_in);
		DWORD ok = 1, info = 0;
		EnterCriticalSection(&g_chamLock);
		if (_code == 0xA410EC60 || _code == 0xA410EC64)
		{
			uint32_t slot = 0;
			if (_code == 0xA410EC60 && _inSize == 4)
			{
				for (uint32_t i = 1; i < 24 && !slot; ++i)
					if (!g_events[i]) { slot = i; g_events[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(in32[0])); ResetEvent(g_events[i]); }
			}
			else if (_code == 0xA410EC64 && _inSize == 8)
			{
				for (uint32_t i = 1; i < 32 && !slot; ++i)
					if (!g_locked[i].va) { slot = i; g_locked[i] = {reinterpret_cast<void*>(static_cast<uintptr_t>(in32[1])), in32[0]}; }
			}
			ok = slot != 0;
			if (ok && _out && _outSize >= 4) { std::memcpy(_out, &slot, 4); info = 4; }
		}
		else if (g_cham != INVALID_SOCKET)
		{
			// 0x5C GetPeriodicStatus copies the status snapshot straight to the user pointer in in[1]
			const bool status = _code == 0xA410EC5C && _inSize >= 8;
			const uint8_t tag = 'I';
			const uint32_t hdr[3] = {_code, _inSize, status ? 0xC0u : _outSize};
			bool good = sendAll(g_cham, &tag, 1) && sendAll(g_cham, hdr, sizeof hdr) && (!_inSize || sendAll(g_cham, _in, int(_inSize)));
			uint32_t rep[3] = {};
			good = good && recvAll(g_cham, rep, sizeof rep);
			std::vector<uint8_t> out(good ? rep[2] : 0);
			good = good && (out.empty() || recvAll(g_cham, out.data(), int(out.size())));
			uint32_t n = 0;
			good = good && recvAll(g_cham, &n, 4);
			for (uint32_t i = 0; good && i < n; ++i)
			{
				uint32_t slot = 0;
				good = recvAll(g_cham, &slot, 4);
				if (good && slot < 24 && g_events[slot]) { SetEvent(g_events[slot]); g_events[slot] = nullptr; }
			}
			good = good && recvAll(g_cham, &n, 4);
			for (uint32_t i = 0; good && i < n; ++i)
			{
				uint32_t hb[2] = {};
				good = recvAll(g_cham, hb, sizeof hb);
				std::vector<uint8_t> data(good ? hb[1] : 0);
				good = good && (data.empty() || recvAll(g_cham, data.data(), int(data.size())));
				if (good && hb[0] < 32 && g_locked[hb[0]].va)
				{
					std::memcpy(g_locked[hb[0]].va, data.data(), std::min<size_t>(data.size(), g_locked[hb[0]].len));
					g_locked[hb[0]] = {};
				}
			}
			if (good)
			{
				ok = rep[0];
				info = rep[1];
				if (status) std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(in32[1])), out.data(), std::min<size_t>(out.size(), 0xC0));
				else if (_out && !out.empty()) std::memcpy(_out, out.data(), std::min<size_t>(out.size(), _outSize));
			}
			else { closesocket(g_cham); g_cham = INVALID_SOCKET; ok = 0; }
		}
		else ok = 0;
		LeaveCriticalSection(&g_chamLock);

		static int logged = 0;
		if (logged++ < 200 || std::getenv("L960_HOOK_VERBOSE"))
			logf("IOCTL %08lx in[%lu] %s-> ok=%lu info=%lx%s", _code, _inSize, hex(_in, _inSize, 24).c_str(), ok, info, _ov ? " (overlapped)" : "");
		if (_ret) *_ret = info;
		completeOverlapped(_ov, info);
		if (!ok) SetLastError(ERROR_GEN_FAILURE);
		return ok ? TRUE : FALSE;
	}

	BOOL WINAPI hkReadFile(HANDLE _h, LPVOID _buf, DWORD _n, LPDWORD _read, LPOVERLAPPED _ov)
	{
		if (!isCom(_h)) return realReadFile(_h, _buf, _n, _read, _ov);
		auto& c = com(_h);
		DWORD got = 0;
		if (c.sock != INVALID_SOCKET && _n)
		{
			// Honour COMMTIMEOUTS roughly: wait up to the total timeout for the first byte, then take what is there
			DWORD wait = c.timeouts.ReadTotalTimeoutConstant + c.timeouts.ReadTotalTimeoutMultiplier * _n;
			if (c.timeouts.ReadIntervalTimeout == MAXDWORD && c.timeouts.ReadTotalTimeoutConstant == 0 && c.timeouts.ReadTotalTimeoutMultiplier == 0) wait = 0;
			if (wait == 0 && c.timeouts.ReadIntervalTimeout != MAXDWORD) wait = 50;
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(c.sock, &fs);
			timeval tv{static_cast<long>(wait / 1000), static_cast<long>((wait % 1000) * 1000)};
			if (select(0, &fs, nullptr, nullptr, &tv) > 0)
			{
				const int k = recv(c.sock, static_cast<char*>(_buf), int(_n), 0);
				if (k > 0) got = DWORD(k);
			}
		}
		if (_read) *_read = got;
		completeOverlapped(_ov, got);
		// Overlapped read with nothing yet: report ERROR_IO_PENDING like a real serial port; LightningInterface treats
		// any other GetLastError() after a FALSE/empty read as fatal and stops its receive thread
		if (_ov && !got) { SetLastError(ERROR_IO_PENDING); return FALSE; }
		static int calls = 0;
		if (std::getenv("L960_HOOK_VERBOSE") && (++calls < 5 || calls % 200 == 0))
			logf("ReadFile#%d(COM%d, n=%lu) got %lu, timeouts I=%lu M=%lu C=%lu", calls, _h == HCom1 ? 1 : 2, _n, got, c.timeouts.ReadIntervalTimeout, c.timeouts.ReadTotalTimeoutMultiplier, c.timeouts.ReadTotalTimeoutConstant);
		static int logged = 0;
		if (got && logged < 40 && std::getenv("L960_HOOK_VERBOSE")) { ++logged; logf("ReadFile(COM, %lu) -> %lu: %s (ov=%p, pRead=%p)", _n, got, hex(_buf, got, 24).c_str(), static_cast<void*>(_ov), static_cast<void*>(_read)); }
		return TRUE;
	}

	BOOL WINAPI hkWriteFile(HANDLE _h, LPCVOID _buf, DWORD _n, LPDWORD _written, LPOVERLAPPED _ov)
	{
		if (!isCom(_h)) return realWriteFile(_h, _buf, _n, _written, _ov);
		auto& c = com(_h);
		if (c.sock != INVALID_SOCKET) sendAll(c.sock, _buf, int(_n));
		static int logged = 0;
		if (logged < 20 && std::getenv("L960_HOOK_VERBOSE")) { ++logged; logf("WriteFile(COM, %lu): %s", _n, hex(_buf, _n, 24).c_str()); }
		if (_written) *_written = _n;
		completeOverlapped(_ov, _n);
		return TRUE;
	}

	BOOL WINAPI hkGetOverlappedResult(HANDLE _h, LPOVERLAPPED _ov, LPDWORD _bytes, BOOL _wait)
	{
		if (_h != HCham && !isCom(_h)) return realGetOverlappedResult(_h, _ov, _bytes, _wait);
		if (_bytes) *_bytes = DWORD(_ov->InternalHigh);
		return TRUE;
	}

	BOOL WINAPI hkCloseHandle(HANDLE _h)
	{
		if (_h == HCham || isCom(_h)) { logf("CloseHandle(virtual %p)", _h); return TRUE; }
		return realCloseHandle(_h);
	}

	BOOL WINAPI hkGetCommState(HANDLE _h, LPDCB _d) { if (isCom(_h)) { *_d = com(_h).dcb; _d->DCBlength = sizeof(DCB); return TRUE; } return GetCommState(_h, _d); }
	BOOL WINAPI hkSetCommState(HANDLE _h, LPDCB _d) { if (isCom(_h)) { com(_h).dcb = *_d; logf("SetCommState(COM, %lu baud)", _d->BaudRate); return TRUE; } return SetCommState(_h, _d); }
	BOOL WINAPI hkSetCommTimeouts(HANDLE _h, LPCOMMTIMEOUTS _t) { if (isCom(_h)) { com(_h).timeouts = *_t; return TRUE; } return SetCommTimeouts(_h, _t); }

	// ------------------------------------------------------------------ virtual registry
	// HKLM\Software\Lexicon\960L and \Cham as on the 960L's NT hive, served from here so nothing has to be
	// imported (and no admin rights are needed). Paths are derived from where Framework.exe lives
	// (<root>\App\Framework.exe); the writable drive is L960_DATA_DIR (default <root>\Data).
	// Set L960_REAL_REGISTRY=1 to use the real registry instead.
	const HKEY HKey960L = reinterpret_cast<HKEY>(0x7F000601);
	const HKEY HKeyCham = reinterpret_cast<HKEY>(0x7F000701);

	std::string rootDir()
	{
		char p[MAX_PATH];
		GetModuleFileNameA(nullptr, p, MAX_PATH);
		std::string s = p;
		for (int i = 0; i < 2; ++i) { const auto k = s.find_last_of("\\/"); if (k != std::string::npos) s.resize(k); }
		return s;
	}

	std::string dataDir()
	{
		std::string d = envOr("L960_DATA_DIR", "");
		if (d.empty()) d = rootDir() + "\\Data";
		while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
		return d;
	}

	struct RegVal { const char* name; DWORD type; std::string str; DWORD num; };

	std::vector<RegVal> regValues(HKEY _k)
	{
		const std::string root = rootDir();
		if (_k == HKey960L)
			// The host expects a drive letter here (a plain directory path stalls it while loading the automation
			// library), so the launcher maps the data directory to a free drive and passes it in L960_DATA_DRIVE.
			return {{"WRITEABLE_DRIVE", REG_SZ, envOr("L960_DATA_DRIVE", dataDir().c_str()), 0},
			        {"HelpFilePath", REG_SZ, root + "\\Diags", 0},
			        {"ShowKeyData", REG_BINARY, "", 0},
			        {"TerminalEnable", REG_BINARY, "", 0},
			        {"TerminalPort", REG_BINARY, "", 2},
			        {"LightningEnable", REG_BINARY, "", 1},
			        {"LightningPort", REG_BINARY, "", 1},
			        {"BoardRev", REG_BINARY, "", 1},
			        {"HaltScript", REG_BINARY, "", 0},
			        {"LogFile", REG_SZ, "Log.txt", 0}};
		if (_k == HKeyCham)
			return {{"DSP56KFile", REG_SZ, root + "\\Cham\\ChamDSP.pci", 0},
			        {"Z80KernelFile", REG_SZ, root + "\\Cham\\Z80OS.PCI", 0},
			        {"ChamIDFile", REG_SZ, "\\960L\\Cham\\ChamID.pci", 0},
			        {"DelayHost", REG_DWORD, "", 0},
			        {"DelayZ80", REG_DWORD, "", 0},
			        {"DelayTMIX", REG_DWORD, "", 0},
			        {"EmployDelayKeStallCPU", REG_DWORD, "", 1},
			        {"EnableCommandThreadOutput", REG_DWORD, "", 0}};
		return {};
	}

	bool useRealRegistry() { return !envOr("L960_REAL_REGISTRY", "").empty(); }

	LONG WINAPI hkRegOpenKeyExA(HKEY _k, LPCSTR _sub, DWORD _o, REGSAM _sam, PHKEY _r)
	{
		if (!useRealRegistry() && _k == HKEY_LOCAL_MACHINE && _sub && _r)
		{
			std::string sub = _sub;
			for (auto& c : sub) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
			while (!sub.empty() && sub.back() == '\\') sub.pop_back();
			if (sub == "SOFTWARE\\LEXICON\\960L") { *_r = HKey960L; logf("RegOpenKeyExA(%s) -> virtual", _sub); return ERROR_SUCCESS; }
			if (sub == "SOFTWARE\\LEXICON\\CHAM") { *_r = HKeyCham; logf("RegOpenKeyExA(%s) -> virtual", _sub); return ERROR_SUCCESS; }
		}
		const LONG rc = RegOpenKeyExA(_k, _sub, _o, _sam, _r);
		logf("RegOpenKeyExA(%p, %s) -> %ld", static_cast<void*>(_k), _sub ? _sub : "", rc);
		return rc;
	}

	LONG WINAPI hkRegQueryValueExA(HKEY _k, LPCSTR _name, LPDWORD _res, LPDWORD _type, LPBYTE _data, LPDWORD _len)
	{
		if (_k == HKey960L || _k == HKeyCham)
		{
			for (const auto& v : regValues(_k))
			{
				if (!_name || _stricmp(_name, v.name) != 0) continue;
				const bool str = v.type == REG_SZ;
				const DWORD need = str ? DWORD(v.str.size() + 1) : 4;
				if (_type) *_type = v.type;
				LONG rc = ERROR_SUCCESS;
				if (_data)
				{
					if (!_len || *_len < need) rc = ERROR_MORE_DATA;
					else if (str) std::memcpy(_data, v.str.c_str(), need);
					else std::memcpy(_data, &v.num, 4);
				}
				if (_len) *_len = need;
				logf("RegQueryValueExA(%s) -> virtual %s", _name, str ? v.str.c_str() : std::to_string(v.num).c_str());
				return rc;
			}
			logf("RegQueryValueExA(%s) -> not in the virtual key", _name ? _name : "");
			return ERROR_FILE_NOT_FOUND;
		}
		const LONG rc = RegQueryValueExA(_k, _name, _res, _type, _data, _len);
		logf("RegQueryValueExA(%s) -> %ld", _name ? _name : "", rc);
		return rc;
	}

	// The host builds paths as %SystemDrive%\960L\... (Larc2\larc2.rom for the LARC2 version check, the
	// algorithm DLLs) and keeps the expanded drive in a buffer only big enough for "C:". The launcher maps a
	// drive letter whose \960L is this copy of the software and passes it in L960_SYSTEM_DRIVE.
	DWORD WINAPI hkExpandEnvironmentStringsA(LPCSTR _src, LPSTR _dst, DWORD _size)
	{
		std::string s = _src ? _src : "";
		const std::string sys = envOr("L960_SYSTEM_DRIVE", "");
		bool changed = false;
		if (!sys.empty())
		{
			std::string u = s;
			for (auto& c : u) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
			const std::string key = "%SYSTEMDRIVE%";
			for (size_t p = u.find(key); p != std::string::npos; p = u.find(key))
			{
				s.replace(p, key.size(), sys);
				u.replace(p, key.size(), sys);
				changed = true;
			}
		}
		const DWORD n = ExpandEnvironmentStringsA(s.c_str(), _dst, _size);
		if (changed) logf("ExpandEnvironmentStringsA(%s) -> %s", _src, _dst && n <= _size ? _dst : s.c_str());
		return n;
	}

	LONG WINAPI hkRegCloseKey(HKEY _k)
	{
		if (_k == HKey960L || _k == HKeyCham) return ERROR_SUCCESS;
		return RegCloseKey(_k);
	}

	HMODULE WINAPI hkLoadLibraryA(LPCSTR _name)
	{
		HMODULE m = realLoadLibraryA(_name);
		if (std::getenv("L960_HOOK_VERBOSE")) logf("LoadLibraryA(%s) -> %p", _name ? _name : "", static_cast<void*>(m));
		if (m) patchModule(m);
		return m;
	}

	// service manager: the driver is "installed" and "started" without touching the system
	SC_HANDLE WINAPI hkOpenSCManagerA(LPCSTR, LPCSTR, DWORD) { return HScm; }
	SC_HANDLE WINAPI hkOpenServiceA(SC_HANDLE, LPCSTR _n, DWORD) { logf("OpenServiceA(%s)", _n ? _n : ""); return HSvc; }
	SC_HANDLE WINAPI hkCreateServiceA(SC_HANDLE, LPCSTR _n, LPCSTR, DWORD, DWORD, DWORD, DWORD, LPCSTR _path, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR)
	{
		logf("CreateServiceA(%s, %s)", _n ? _n : "", _path ? _path : "");
		return HSvc;
	}
	BOOL WINAPI hkStartServiceA(SC_HANDLE, DWORD, LPCSTR*) { logf("StartServiceA"); return TRUE; }
	BOOL WINAPI hkControlService(SC_HANDLE, DWORD _c, LPSERVICE_STATUS _s) { if (_s) std::memset(_s, 0, sizeof *_s); logf("ControlService(%lu)", _c); return TRUE; }
	BOOL WINAPI hkDeleteService(SC_HANDLE) { return TRUE; }
	BOOL WINAPI hkCloseServiceHandle(SC_HANDLE) { return TRUE; }

	// ChamAPI only drives the card on Windows NT 4 (GetChamWinVer == 3); report the 960L's own OS
	BOOL WINAPI hkGetVersionExA(LPOSVERSIONINFOA _v)
	{
		if (!_v || _v->dwOSVersionInfoSize < sizeof(OSVERSIONINFOA)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
		const DWORD size = _v->dwOSVersionInfoSize;
		std::memset(_v, 0, size);
		_v->dwOSVersionInfoSize = size;
		_v->dwMajorVersion = 4;
		_v->dwMinorVersion = 0;
		_v->dwBuildNumber = 1381;
		_v->dwPlatformId = VER_PLATFORM_WIN32_NT;
		std::strcpy(_v->szCSDVersion, "Service Pack 6");
		return TRUE;
	}

	struct Hook { const char* dll; const char* name; void* fn; };
	const Hook g_hooks[] = {
		{"KERNEL32.DLL", "CreateFileA", reinterpret_cast<void*>(&hkCreateFileA)},
		{"KERNEL32.DLL", "DeviceIoControl", reinterpret_cast<void*>(&hkDeviceIoControl)},
		{"KERNEL32.DLL", "ReadFile", reinterpret_cast<void*>(&hkReadFile)},
		{"KERNEL32.DLL", "WriteFile", reinterpret_cast<void*>(&hkWriteFile)},
		{"KERNEL32.DLL", "GetOverlappedResult", reinterpret_cast<void*>(&hkGetOverlappedResult)},
		{"KERNEL32.DLL", "CloseHandle", reinterpret_cast<void*>(&hkCloseHandle)},
		{"KERNEL32.DLL", "GetCommState", reinterpret_cast<void*>(&hkGetCommState)},
		{"KERNEL32.DLL", "SetCommState", reinterpret_cast<void*>(&hkSetCommState)},
		{"KERNEL32.DLL", "SetCommTimeouts", reinterpret_cast<void*>(&hkSetCommTimeouts)},
		{"KERNEL32.DLL", "GetVersionExA", reinterpret_cast<void*>(&hkGetVersionExA)},
		{"KERNEL32.DLL", "ExpandEnvironmentStringsA", reinterpret_cast<void*>(&hkExpandEnvironmentStringsA)},
		{"KERNEL32.DLL", "LoadLibraryA", reinterpret_cast<void*>(&hkLoadLibraryA)},
		{"ADVAPI32.DLL", "RegOpenKeyExA", reinterpret_cast<void*>(&hkRegOpenKeyExA)},
		{"ADVAPI32.DLL", "RegQueryValueExA", reinterpret_cast<void*>(&hkRegQueryValueExA)},
		{"ADVAPI32.DLL", "RegCloseKey", reinterpret_cast<void*>(&hkRegCloseKey)},
		{"ADVAPI32.DLL", "OpenSCManagerA", reinterpret_cast<void*>(&hkOpenSCManagerA)},
		{"ADVAPI32.DLL", "OpenServiceA", reinterpret_cast<void*>(&hkOpenServiceA)},
		{"ADVAPI32.DLL", "CreateServiceA", reinterpret_cast<void*>(&hkCreateServiceA)},
		{"ADVAPI32.DLL", "StartServiceA", reinterpret_cast<void*>(&hkStartServiceA)},
		{"ADVAPI32.DLL", "ControlService", reinterpret_cast<void*>(&hkControlService)},
		{"ADVAPI32.DLL", "DeleteService", reinterpret_cast<void*>(&hkDeleteService)},
		{"ADVAPI32.DLL", "CloseServiceHandle", reinterpret_cast<void*>(&hkCloseServiceHandle)},
	};

	HMODULE g_self = nullptr;

	void patchModule(HMODULE _m)
	{
		if (_m == g_self) return;
		auto* base = reinterpret_cast<uint8_t*>(_m);
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) return;
		for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
		{
			std::string dll = reinterpret_cast<const char*>(base + d->Name);
			for (auto& c : dll) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
			if (!d->OriginalFirstThunk) continue;
			auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
			auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++iat)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
				const char* fn = reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name);
				for (const auto& h : g_hooks)
				{
					if (dll != h.dll || std::strcmp(fn, h.name) != 0) continue;
					DWORD old;
					VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
					iat->u1.Function = reinterpret_cast<ULONG_PTR>(h.fn);
					VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
				}
			}
		}
	}

	void install()
	{
		const HMODULE k32 = GetModuleHandleA("kernel32.dll");
		realCreateFileA = reinterpret_cast<CreateFileA_t>(GetProcAddress(k32, "CreateFileA"));
		realDeviceIoControl = reinterpret_cast<DeviceIoControl_t>(GetProcAddress(k32, "DeviceIoControl"));
		realReadFile = reinterpret_cast<ReadFile_t>(GetProcAddress(k32, "ReadFile"));
		realWriteFile = reinterpret_cast<WriteFile_t>(GetProcAddress(k32, "WriteFile"));
		realGetOverlappedResult = reinterpret_cast<GetOverlappedResult_t>(GetProcAddress(k32, "GetOverlappedResult"));
		realCloseHandle = reinterpret_cast<CloseHandle_t>(GetProcAddress(k32, "CloseHandle"));
		realLoadLibraryA = reinterpret_cast<LoadLibraryA_t>(GetProcAddress(k32, "LoadLibraryA"));

		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		MODULEENTRY32 me{};
		me.dwSize = sizeof me;
		int n = 0;
		if (snap != INVALID_HANDLE_VALUE && Module32First(snap, &me))
			do { patchModule(me.hModule); ++n; } while (Module32Next(snap, &me));
		if (snap != INVALID_HANDLE_VALUE) realCloseHandle(snap);
		logf("l960hook installed, %d modules patched", n);
	}
}

BOOL WINAPI DllMain(HINSTANCE _inst, DWORD _reason, LPVOID)
{
	if (_reason == DLL_PROCESS_ATTACH)
	{
		g_self = _inst;
		InitializeCriticalSection(&g_logLock);
		InitializeCriticalSection(&g_chamLock);
		InitializeCriticalSection(&g_comLock[0]);
		InitializeCriticalSection(&g_comLock[1]);
		DisableThreadLibraryCalls(_inst);
		g_logPath = dataDir() + "\\l960hook.log";
		CreateDirectoryA(dataDir().c_str(), nullptr);
		install();
	}
	return TRUE;
}
