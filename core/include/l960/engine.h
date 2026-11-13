// Top-level emulator used by the plugin and the standalone app.
//
// Two modes:
//  - DirectLexichip: algorithm microcode is loaded straight into the Lexichip models (as ChamAPI does)
//    and plugin audio is fed into the Lexichips' T-MIX serial slots. No 56301/Z80 firmware runs, so
//    coefficients stay at the values stored in the algorithm DLL.
//  - HostDriven: the original 960L host software (helper process, see HostSystem) programs the card through
//    the emulated driver and firmware; programs and live coefficients arrive via the Z80s. Plugin audio
//    goes to machine A (Lexichips 1 and 2). The T-MIX routing is not modelled yet; the serial slots used
//    are detected from the running program.
#pragma once

#include "l960/card.h"
#include "l960/firmware.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace l960
{
	class Engine
	{
	public:
		enum class Mode { DirectLexichip, HostDriven };

		Engine();

		bool loadFirmware(const std::filesystem::path& _root, std::string* _error = nullptr);
		const Firmware* firmware() const { return m_fw.get(); }

		// Not real-time safe: call from the message thread with audio stopped or behind a lock
		bool selectAlgorithm(const std::string& _name, double _sampleRate, std::string* _error = nullptr);
		const std::string& currentAlgorithm() const { return m_algName; }

		void setMode(Mode _m);
		Mode mode() const { return m_mode; }

		// Real-time: interleaving-free float buffers, +-1.0 full scale
		void process(const float* const* _in, int _numIn, float* const* _out, int _numOut, int _numSamples);

		ReverbCard& card() { return *m_card; }
		// Held while audio is processed; whoever runs the card's control processors takes it per slice
		std::mutex& cardLock() { return m_cardLock; }

		// Serial slots auto-detected from the loaded program (T-MIX side inputs/outputs per chip)
		struct ChipIo { std::vector<uint32_t> inSlots, outSlots; };
		const std::vector<ChipIo>& io() const { return m_io; }

		// Host-driven routing: which backplane interfaces carry the plugin's audio (the analog cards by default)
		void setHostInterfaces(int _in, int _out) { m_ifIn = _in; m_ifOut = _out; }

	private:
		void detectIo(size_t _chip);
		void processHost(const float* const* _in, int _numIn, float* const* _out, int _numOut, int _numSamples);
		void hostSample(const float* _in, int _numIn, float* _out, int _numOut);

		std::unique_ptr<Firmware> m_fw;
		std::unique_ptr<ReverbCard> m_card;
		Mode m_mode = Mode::DirectLexichip;
		std::string m_algName;
		size_t m_chipsUsed = 0;
		std::vector<ChipIo> m_io;
		std::mutex m_cardLock;
		uint64_t m_structureSeen = ~0ull;

		// 56301 mixer kernel state (HLE): slewed working copies of the host's coefficient targets
		std::array<int32_t, 128> m_min{}, m_mout{};
		std::array<int32_t, 8> m_gsend{}, m_gret{};
		uint32_t m_coefRefresh = 0;
		std::array<int32_t, 128> m_tMin{}, m_tMout{};
		std::array<int32_t, 8> m_tGsend{}, m_tGret{};
		int m_ifIn = 5, m_ifOut = 4;
	};
}
