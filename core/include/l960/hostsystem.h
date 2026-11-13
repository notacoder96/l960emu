// Runs the original 960L host software against the emulated hardware.
//
//   helper process (32-bit, Windows):  Framework.exe + host DLLs + hosthelper/Enums.dll hook
//        |  TCP 127.0.0.1: CHAM IOCTLs, COM1 (LARC2), COM2 (terminal)
//   HostSystem:  HostBridge  ->  ChamDevice (CHAM.sys)  ->  ReverbCard (56301, Z80s, Lexichips, I/O bus)
//                            ->  Larc2Device (the emulated remote and its screen)
//
// On Windows, launchHelper() prepares a private copy of the 960L folder with the hook installed, maps the
// host's writable drive to a data folder and starts Framework.exe hidden. On other systems a command can be
// given in L960_HELPER_CMD (for example a wine invocation); otherwise start the helper yourself.
#pragma once

#include "l960/card.h"
#include "l960/chamdevice.h"
#include "l960/hostbridge.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace l960
{
	namespace larc2 { class Larc2Device; }

	class HostSystem
	{
	public:
		HostSystem(ReverbCard& _card, std::mutex& _cardLock, larc2::Larc2Device& _larc2);
		~HostSystem();

		// Starts the bridge (first free base port from 7960 in steps of 10) and the idle thread.
		// _root: the "960L" folder (needs Cham/ChamID.pci).
		bool start(const std::filesystem::path& _root, std::string* _error = nullptr);
		void stop();
		bool running() const { return m_bridge != nullptr; }
		int basePort() const { return m_port; }

		// Starts the original host software. _dataDir holds its writable drive (presets, sessions, logs).
		bool launchHelper(const std::filesystem::path& _root, const std::filesystem::path& _dataDir, std::string* _error = nullptr);
		void stopHelper();
		bool helperRunning() const;

		std::string status() const;
		std::function<void(const std::string&)> onLog;

	private:
		void log(const std::string& _s) { if (onLog) onLog(_s); }

		ReverbCard& m_card;
		std::mutex& m_cardLock;
		larc2::Larc2Device& m_larc2;
		std::unique_ptr<HostBridge> m_bridge;
		std::unique_ptr<ChamDevice> m_cham;
		std::mutex m_chamLock;  // serialises IOCTLs and idle slices
		std::thread m_idle;
		std::atomic<bool> m_quit{false};
		int m_port = 0;

		struct Helper;
		std::unique_ptr<Helper> m_helper;
	};
}
