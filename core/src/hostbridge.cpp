#include "l960/hostbridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK ::close
#endif

#include <cstring>

namespace l960
{
	namespace
	{
		bool recvAll(int _s, void* _p, size_t _n)
		{
			auto* p = static_cast<char*>(_p);
			while (_n)
			{
				const int k = ::recv(_s, p, int(_n), 0);
				if (k <= 0) return false;
				p += k;
				_n -= size_t(k);
			}
			return true;
		}

		bool sendAll(int _s, const void* _p, size_t _n)
		{
			auto* p = static_cast<const char*>(_p);
			while (_n)
			{
				const int k = ::send(_s, p, int(_n), 0);
				if (k <= 0) return false;
				p += k;
				_n -= size_t(k);
			}
			return true;
		}
	}

	HostBridge::HostBridge(const int _basePort) : m_base(_basePort) {}
	HostBridge::~HostBridge() { stop(); }

	bool HostBridge::start(std::string* _error)
	{
#ifdef _WIN32
		static bool wsa = false;
		if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
#endif
		m_quit = false;
		for (int i = 0; i < 3; ++i)
		{
			const int s = int(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
			int one = 1;
			::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(uint16_t(m_base + i));
			a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 2) != 0)
			{
				if (_error) *_error = "cannot listen on 127.0.0.1:" + std::to_string(m_base + i);
				CLOSESOCK(s);
				stop();
				return false;
			}
			m_listen.push_back(s);
			m_threads.emplace_back([this, s, i] { acceptLoop(s, i); });
		}
		return true;
	}

	void HostBridge::stop()
	{
		m_quit = true;
		for (int s : m_listen) CLOSESOCK(s);
		if (int c = m_comSock.exchange(-1); c >= 0) CLOSESOCK(c);
		for (auto& t : m_threads) if (t.joinable()) t.join();
		m_threads.clear();
		m_listen.clear();
	}

	void HostBridge::acceptLoop(const int _listen, const int _which)
	{
		while (!m_quit)
		{
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(_listen, &fs);
			timeval tv{0, 200000};
			if (::select(_listen + 1, &fs, nullptr, nullptr, &tv) <= 0) continue;
			const int c = int(::accept(_listen, nullptr, nullptr));
			if (c < 0) continue;
			int one = 1;
			::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
			if (onLog) onLog("host connected on port " + std::to_string(m_base + _which));
			if (_which == 0) serveCham(c);
			else serveCom(c, _which);
			CLOSESOCK(c);
			if (onLog) onLog("host disconnected from port " + std::to_string(m_base + _which));
		}
	}

	void HostBridge::serveCham(const int _sock)
	{
		while (!m_quit)
		{
			uint8_t tag;
			if (!recvAll(_sock, &tag, 1)) return;
			if (tag == 'O')
			{
				const uint32_t ok = onOpen ? (onOpen() ? 1u : 0u) : 1u;
				if (!sendAll(_sock, &ok, 4)) return;
				continue;
			}
			uint32_t hdr[3];
			if (tag != 'I' || !recvAll(_sock, hdr, sizeof hdr)) return;
			Ioctl req;
			req.code = hdr[0];
			req.outSize = hdr[2];
			req.in.resize(hdr[1]);
			if (hdr[1] && !recvAll(_sock, req.in.data(), hdr[1])) return;
			++m_ioctls;
			IoctlReply rep;
			if (onIoctl) rep = onIoctl(req);
			else { rep.out.assign(req.outSize, 0); rep.info = req.outSize; }

			std::vector<uint8_t> msg;
			auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) msg.push_back(uint8_t(v >> (8 * i))); };
			u32(rep.ok ? 1u : 0u);
			u32(rep.info);
			u32(uint32_t(rep.out.size()));
			msg.insert(msg.end(), rep.out.begin(), rep.out.end());
			u32(uint32_t(rep.events.size()));
			for (uint32_t e : rep.events) u32(e);
			u32(uint32_t(rep.readbacks.size()));
			for (const auto& r : rep.readbacks)
			{
				u32(r.mdl);
				u32(uint32_t(r.bytes.size()));
				msg.insert(msg.end(), r.bytes.begin(), r.bytes.end());
			}
			if (!sendAll(_sock, msg.data(), msg.size())) return;
		}
	}

	void HostBridge::serveCom(const int _sock, const int _which)
	{
		if (_which == 1) m_comSock = _sock;
		std::vector<uint8_t> buf(4096);
		std::string term;
		while (!m_quit)
		{
			fd_set fs;
			FD_ZERO(&fs);
			FD_SET(_sock, &fs);
			timeval tv{0, 200000};
			const int r = ::select(_sock + 1, &fs, nullptr, nullptr, &tv);
			if (r < 0) break;
			if (r == 0) continue;
			const int k = ::recv(_sock, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
			if (k <= 0) break;
			if (_which == 1) { if (onLarc2Data) onLarc2Data(buf.data(), size_t(k)); }
			else
				for (int i = 0; i < k; ++i)
				{
					if (buf[i] == '\n' || buf[i] == '\r') { if (!term.empty() && onLog) onLog("[terminal] " + term); term.clear(); }
					else if (buf[i] >= 32 && buf[i] < 127) term.push_back(char(buf[i]));
				}
		}
		if (_which == 1) m_comSock = -1;
	}

	void HostBridge::sendToHost(const uint8_t* _data, const size_t _n)
	{
		const int s = m_comSock.load();
		if (s < 0) return;
		std::lock_guard<std::mutex> l(m_sendLock);
		sendAll(s, _data, _n);
	}
}
