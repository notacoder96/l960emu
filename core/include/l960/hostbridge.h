// TCP bridge between the emulator and the 960L host software running in the 32-bit helper (hosthelper/).
//
// The helper's hook DLL turns the host's hardware into three TCP connections to 127.0.0.1:
//   7960  CHAM driver: request  'O'                                             (CreateFile)
//                      reply    u32 ok
//                      request  'I' u32 ioctl, u32 inSize, u32 outSize, in[inSize]   (DeviceIoControl)
//                      reply    u32 ok, u32 info, u32 outBytes, out[outBytes],
//                               u32 nEvents, u32 slot[nEvents],                     event slots to signal
//                               u32 nReadbacks, {u32 mdl, u32 n, bytes[n]}...       locked-buffer writes
//   7961  COM1 = LARC2 link (raw bytes both ways)
//   7962  COM2 = service terminal (raw bytes; logged)
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace l960
{
	class HostBridge
	{
	public:
		struct Ioctl { uint32_t code = 0; std::vector<uint8_t> in; uint32_t outSize = 0; };
		struct Readback { uint32_t mdl = 0; std::vector<uint8_t> bytes; };
		struct IoctlReply
		{
			bool ok = true;
			uint32_t info = 0;  // IRP Information (bytes returned / ERRVAL)
			std::vector<uint8_t> out;
			std::vector<uint32_t> events;
			std::vector<Readback> readbacks;
		};

		explicit HostBridge(int _basePort = 7960);
		~HostBridge();

		bool start(std::string* _error = nullptr);
		void stop();

		// Called on bridge threads
		std::function<bool()> onOpen;
		std::function<IoctlReply(const Ioctl&)> onIoctl;
		std::function<void(const uint8_t*, size_t)> onLarc2Data;  // host -> LARC2
		std::function<void(const std::string&)> onLog;

		// LARC2 -> host (thread-safe)
		void sendToHost(const uint8_t* _data, size_t _n);

		bool hostConnected() const { return m_comSock.load() >= 0; }
		uint64_t ioctlCount() const { return m_ioctls.load(); }

	private:
		void acceptLoop(int _listen, int _which);
		void serveCham(int _sock);
		void serveCom(int _sock, int _which);

		int m_base;
		std::vector<int> m_listen;
		std::vector<std::thread> m_threads;
		std::atomic<bool> m_quit{false};
		std::atomic<int> m_comSock{-1};
		std::mutex m_sendLock;
		std::atomic<uint64_t> m_ioctls{0};
	};
}
