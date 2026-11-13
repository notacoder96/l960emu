#include "l960/engine.h"

#include <algorithm>
#include <cmath>

namespace l960
{
	namespace
	{
		constexpr float FullScale = 8388608.0f;  // 2^23

		// Chip-to-chip link (odd "LEX" registers): output channel c (reg $41+2c) arrives on the partner's input
		// channel (c + 2) mod 8 (reg $51 + 2c'); see docs/LEXICHIP.md
		uint32_t lexLink(const uint32_t _outSlot) { return 16 + 2 * (((_outSlot >> 1) + 2) & 7) + 1; }

		int32_t toFixed(const float _v)
		{
			const float s = std::clamp(_v, -1.0f, 1.0f - 1.0f / FullScale) * FullScale;
			return static_cast<int32_t>(std::lrint(s));
		}
	}

	Engine::Engine() : m_card(std::make_unique<ReverbCard>()) {}

	void Engine::setMode(const Mode _m)
	{
		std::lock_guard<std::mutex> l(m_cardLock);
		m_mode = _m;
		if (_m == Mode::HostDriven)
		{
			m_io.assign(2, {});
			for (auto& io : m_io) { io.inSlots.reserve(16); io.outSlots.reserve(16); }
			m_chipsUsed = 2;
			m_algName = "960L host";
			m_structureSeen = ~0ull;
		}
	}

	bool Engine::loadFirmware(const std::filesystem::path& _root, std::string* _error)
	{
		auto fw = std::make_unique<Firmware>();
		if (!fw->load(_root, _error)) return false;
		m_fw = std::move(fw);
		return true;
	}

	bool Engine::selectAlgorithm(const std::string& _name, const double _sampleRate, std::string* _error)
	{
		if (!m_fw) { if (_error) *_error = "no firmware loaded"; return false; }
		const auto* alg = m_fw->findAlgorithm(_name);
		if (!alg) { if (_error) *_error = "unknown algorithm " + _name; return false; }

		const bool dbl = _sampleRate > 50000.0;
		const auto& progs = dbl ? alg->doubleSpeed : alg->singleSpeed;
		if (progs.empty()) { if (_error) *_error = alg->name + " has no program for this sample rate"; return false; }
		if (progs.size() > 4) { if (_error) *_error = "program needs more than 4 Lexichips"; return false; }

		std::lock_guard<std::mutex> l(m_cardLock);
		m_mode = Mode::DirectLexichip;
		m_card->reset();
		m_io.assign(progs.size(), {});
		for (auto& io : m_io) { io.inSlots.reserve(16); io.outSlots.reserve(16); }
		for (size_t i = 0; i < progs.size(); ++i)
		{
			m_card->lexichip(i).loadUcode(progs[i]);
			detectIo(i);
		}
		m_chipsUsed = progs.size();
		m_algName = alg->name;
		return true;
	}

	void Engine::detectIo(const size_t _chip)
	{
		// allocation-free: also runs on the audio thread in host-driven mode
		auto& lx = m_card->lexichip(_chip);
		uint32_t in = 0, out = 0;  // bit n = slot n
		auto note = [&](uint8_t r, bool isWrite) {
			if (r >= Lexichip::RegIn0 && r < Lexichip::RegIn0 + 16 && !(r & 1) && !isWrite) in |= 1u << (r - Lexichip::RegIn0 + 16);
			if (r >= Lexichip::RegOut0 && r < Lexichip::RegOut0 + 16 && !(r & 1) && isWrite) out |= 1u << (r - Lexichip::RegOut0);
		};
		uint32_t q = 0;
		for (size_t i = 0; i < lx.programLength(); ++i)
		{
			const auto& o = lx.op(i);
			if (o.mem)
			{
				const auto a = static_cast<uint16_t>(Lexichip::XmoqBase + 4 * q++);
				const auto e = XmoqEntry::decode(lx.busRead(a) | (lx.busRead(a + 1) << 8) | (lx.busRead(a + 2) << 16) |
				                                 (static_cast<uint32_t>(lx.busRead(a + 3)) << 24));
				note(e.reg, !e.write);
			}
			if (!o.mulAcc) note(o.src, false);
			if (o.dst < Lexichip::RegNone) note(o.dst, true);
		}
		auto& io = m_io[_chip];
		io.inSlots.clear();
		io.outSlots.clear();
		for (uint32_t b = 0; b < 32; ++b)
		{
			if (in & (1u << b)) io.inSlots.push_back(b);
			if (out & (1u << b)) io.outSlots.push_back(b);
		}
	}

	void Engine::process(const float* const* _in, const int _numIn, float* const* _out, const int _numOut, const int _numSamples)
	{
		for (int c = 0; c < _numOut; ++c) std::fill(_out[c], _out[c] + _numSamples, 0.0f);
		std::lock_guard<std::mutex> l(m_cardLock);
		if (m_chipsUsed == 0) return;
		if (m_mode == Mode::HostDriven)
		{
			processHost(_in, _numIn, _out, _numOut, _numSamples);
			return;
		}

		for (int n = 0; n < _numSamples; ++n)
		{
			// Inputs: plugin channel k -> the k-th T-MIX input slot found across the chips in use
			int ch = 0;
			for (size_t c = 0; c < m_chipsUsed; ++c)
				for (const auto slot : m_io[c].inSlots)
				{
					const float v = _numIn > 0 ? _in[std::min(ch, _numIn - 1)][n] : 0.0f;
					m_card->lexichip(c).setSerialIn(slot, toFixed(v));
					++ch;
				}

			for (size_t c = 0; c < m_chipsUsed; ++c) m_card->lexichip(c).processSample();

			// ASSUMPTION: chips are chained in pairs (1<->2, 3<->4); odd "LEX" output slot n feeds the
			// partner's LEX input slot n + 16 on the next sample
			for (size_t c = 0; c + 1 < m_chipsUsed; c += 2)
				for (uint32_t s = 1; s < 16; s += 2)
				{
					auto& a = m_card->lexichip(c);
					auto& b = m_card->lexichip(c + 1);
					const int32_t ab = a.serialOut(s), ba = b.serialOut(s);
					b.setSerialIn(lexLink(s), ab);
					a.setSerialIn(lexLink(s), ba);
				}

			ch = 0;
			for (size_t c = 0; c < m_chipsUsed; ++c)
				for (const auto slot : m_io[c].outSlots)
				{
					if (ch < _numOut) _out[ch][n] = static_cast<float>(m_card->lexichip(c).serialOut(slot)) / FullScale;
					++ch;
				}
		}
	}

	// ------------------------------------------------------------------ host-driven card audio
	//
	// Model of the reverb card's audio path (docs/AUDIO_PATH.md):
	//   I/O backplane -> T-MIX1 PPRAM -> 56301 mixer kernel (HLE) -> T-MIX3 PPRAM -> Lexichips (TMX slots)
	//   Lexichips -> T-MIX3 PPRAM -> 56301 kernel -> T-MIX2 PPRAM -> I/O backplane
	// T-MIX map register $A0 + 8*port + channel = PPRAM location ($FE = zero, $FF = none); backplane interface n is
	// T-MIX port pair (2n, 2n+1), card channel i = line (i & 1), slot (i >> 1). T-MIX3 ports 0-3 feed Lexichip 1-4,
	// ports 4-7 receive them; channel c <-> TMX slot (c + 1) mod 8.

	namespace
	{
		constexpr double Frac = 8388608.0;

		int32_t s24(uint32_t _v) { return static_cast<int32_t>(_v << 8) >> 8; }

		int32_t satFrac(double _v)
		{
			const double s = std::round(_v * Frac);
			return static_cast<int32_t>(std::clamp(s, -Frac, Frac - 1));
		}

		void slew(int32_t& _w, const int32_t _t)
		{
			const int32_t step = std::abs(_w) / 256 + 0x400;
			if (_w < _t) _w = std::min(_w + step, _t);
			else if (_w > _t) _w = std::max(_w - step, _t);
		}

		uint8_t mapEntry(const Tmix& _t, uint32_t _port, uint32_t _ch) { return _t.reg(static_cast<uint8_t>(0xA0 + 8 * _port + _ch)); }
	}

	void Engine::processHost(const float* const* _in, const int _numIn, float* const* _out, const int _numOut, const int _numSamples)
	{
		// Coefficient targets the host poked into the 56301 (stored negated), refreshed per block
		auto& dsp = m_card->dsp();
		for (uint32_t i = 0; i < 128; ++i) { m_tMin[i] = s24(dsp.readX(0x100 + i)); m_tMout[i] = s24(dsp.readX(0x188 + i)); }
		for (uint32_t i = 0; i < 8; ++i) { m_tGsend[i] = s24(dsp.readX(0x180 + i)); m_tGret[i] = s24(dsp.readY(0x150 + i)); }

		float in[8], out[8];
		for (int n = 0; n < _numSamples; ++n)
		{
			for (int c = 0; c < 8; ++c) in[c] = c < _numIn ? _in[c][n] : 0.0f;
			hostSample(in, 8, out, 8);
			for (int c = 0; c < _numOut && c < 8; ++c) _out[c][n] = out[c];
		}
	}

	void Engine::hostSample(const float* _in, const int _numIn, float* _out, const int _numOut)
	{
		auto& t1 = m_card->tmix(0);
		auto& t2 = m_card->tmix(1);
		auto& t3 = m_card->tmix(2);
		auto pp = [](Tmix& _t, uint32_t _v) -> int32_t { return _v >= 0xFE ? 0 : s24(_t.dspRead(0x300 + _v)); };
		auto ppw = [](Tmix& _t, uint32_t _v, int32_t _x) { if (_v < 0xFE) _t.dspWrite(0x300 + _v, static_cast<uint32_t>(_x)); };

		// 1. backplane inputs -> T-MIX1 (the plugin's inputs on interface m_ifIn)
		for (int i = 0; i < 8; ++i)
		{
			const uint32_t port = 2u * uint32_t(m_ifIn) + (i & 1), ch = uint32_t(i >> 1);
			const float v = i < _numIn ? _in[i] : 0.0f;
			ppw(t1, mapEntry(t1, port, ch), toFixed(v));
		}
		// 2. Lexichip TMX outputs (previous sample) -> T-MIX3 ports 4-7
		for (uint32_t chip = 0; chip < 4; ++chip)
			for (uint32_t c = 0; c < 8; ++c)
			{
				const uint8_t v = mapEntry(t3, 4 + chip, c);
				if (v < 0xFE) ppw(t3, v, m_card->lexichip(chip).serialOut(2 * ((c + 7) & 7)));
			}

		// 3. 56301 mixer kernel (8-input mode)
		for (uint32_t i = 0; i < 128; ++i) { slew(m_min[i], m_tMin[i]); slew(m_mout[i], m_tMout[i]); }
		for (uint32_t i = 0; i < 8; ++i) { slew(m_gsend[i], m_tGsend[i]); slew(m_gret[i], m_tGret[i]); }
		double in[8], r[16];
		for (uint32_t i = 0; i < 8; ++i) in[i] = s24(t1.dspRead(0x380 + i)) / Frac;
		for (uint32_t j = 0; j < 16; ++j)
		{
			double s = 0;
			for (uint32_t i = 0; i < 8; ++i) s += -m_min[16 * (j >> 1) + 2 * i + (j & 1)] / Frac * in[i];
			const double send = -m_gsend[j >> 1] / Frac * s;
			t3.dspWrite(0x390 + j, static_cast<uint32_t>(satFrac(send)) & 0xFFFFFF);
		}
		for (uint32_t k = 0; k < 16; ++k)
		{
			r[k] = -m_gret[k >> 1] / Frac * (s24(t3.dspRead(0x380 + k)) / Frac);
			t1.dspWrite(0x390 + k, static_cast<uint32_t>(satFrac(r[k])) & 0xFFFFFF);  // wet output
		}
		double o[16];
		for (uint32_t m = 0; m < 16; ++m)
		{
			o[m] = s24(t2.dspRead(0x380 + m)) / Frac;
			for (uint32_t k = 0; k < 8; ++k) o[m] += -m_mout[16 * (m >> 1) + 2 * k + (m & 1)] / Frac * r[(m >> 3) * 8 + k];
		}
		for (uint32_t m = 0; m < 8; ++m) t2.dspWrite(0x390 + m, static_cast<uint32_t>(satFrac(o[m] + o[m + 8])) & 0xFFFFFF);

		// 4. T-MIX3 ports 0-3 -> Lexichip TMX inputs; run the chips; LEX chaining on the odd registers
		for (uint32_t chip = 0; chip < 4; ++chip)
			for (uint32_t c = 0; c < 8; ++c)
			{
				const uint8_t v = mapEntry(t3, chip, c);
				if (v != 0xFF) m_card->lexichip(chip).setSerialIn(16 + 2 * ((c + 1) & 7), pp(t3, v));
			}
		for (uint32_t chip = 0; chip < 4; ++chip) m_card->lexichip(chip).processSample();
		for (uint32_t c = 0; c < 4; c += 2)
			for (uint32_t s = 1; s < 16; s += 2)
			{
				auto& a = m_card->lexichip(c);
				auto& b = m_card->lexichip(c + 1);
				const int32_t ab = a.serialOut(s), ba = b.serialOut(s);
				b.setSerialIn(lexLink(s), ab);
				a.setSerialIn(lexLink(s), ba);
			}

		// 5. T-MIX2 -> backplane outputs (the plugin's outputs on interface m_ifOut)
		for (int i = 0; i < _numOut; ++i)
		{
			const uint32_t port = 2u * uint32_t(m_ifOut) + (i & 1), ch = uint32_t(i >> 1) & 3;
			_out[i] = static_cast<float>(pp(t2, mapEntry(t2, port, ch)) / Frac);
		}
	}
}
