#include "l960/firmware.h"
#include "l960/pe32.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;

namespace l960
{
	namespace
	{
		bool fail(std::string* _e, std::string _msg)
		{
			if (_e) *_e = std::move(_msg);
			return false;
		}

		std::string lower(std::string _s)
		{
			std::transform(_s.begin(), _s.end(), _s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return _s;
		}

		// The CD was mastered on Windows; file name case is inconsistent (ChamDSP.pci vs Z80OS.PCI).
		fs::path findCI(const fs::path& _dir, const std::string& _name)
		{
			std::error_code ec;
			if (fs::exists(_dir / _name, ec)) return _dir / _name;
			const auto want = lower(_name);
			for (const auto& e : fs::directory_iterator(_dir, ec))
				if (lower(e.path().filename().string()) == want) return e.path();
			return {};
		}

		// ALG_INIT_STRUCT field offsets (32-bit DLL layout)
		constexpr uint32_t AlgNameOffset = 0x10;    // char* name
		constexpr uint32_t AlgSpeedsOffset = 0x4C;  // UCODE_SET* speeds[2] (single, double)

		bool readUcodeSet(const Pe32Image& _pe, const uint32_t _setVA, std::vector<Ucode>& _out, std::string* _e)
		{
			// UCODE_SET: {u32 count; UCODE** list}
			const auto count = _pe.u32(_setVA), list = _pe.u32(_setVA + 4);
			if (!count || !list || *count > 4) return fail(_e, "bad UCODE set");
			for (uint32_t i = 0; i < *count; ++i)
			{
				const auto uc = _pe.u32(*list + 4 * i);
				if (!uc) return fail(_e, "bad UCODE pointer");
				// UCODE: {u16 ?; u16 lines; u8* wcs; u8 xmoqCount; (pad); u32* xmoq}
				const auto lines = _pe.u16(*uc + 2);
				const auto wcsPtr = _pe.u32(*uc + 4);
				const auto nX = _pe.u8(*uc + 8);
				const auto xPtr = _pe.u32(*uc + 12);
				if (!lines || !wcsPtr || !nX || !xPtr || *lines == 0 || *lines > 256) return fail(_e, "bad UCODE header");
				Ucode u;
				u.lines = *lines;
				for (size_t p = 0; p < Ucode::PlaneCount; ++p)
				{
					u.wcs[p].resize(u.lines);
					if (!_pe.read(*wcsPtr + static_cast<uint32_t>(p * u.lines), u.wcs[p].data(), u.lines))
						return fail(_e, "WCS out of range");
				}
				u.xmoq.resize(*nX);
				if (*nX && !_pe.read(*xPtr, u.xmoq.data(), *nX * 4u)) return fail(_e, "XMOQ out of range");
				_out.push_back(std::move(u));
			}
			return true;
		}

		bool loadBootImage(const fs::path& _dir, const std::string& _name, Dsp56kBootImage& _img, std::string* _e)
		{
			const auto p = findCI(_dir, _name);
			std::vector<uint8_t> data;
			if (p.empty() || !readFile(p, data)) return fail(_e, "missing " + _name);
			std::string err;
			if (!_img.parse(data, &err)) return fail(_e, _name + ": " + err);
			return true;
		}
	}

	bool readFile(const fs::path& _file, std::vector<uint8_t>& _out)
	{
		std::ifstream f(_file, std::ios::binary);
		if (!f) return false;
		_out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}

	bool Dsp56kBootImage::parse(const std::vector<uint8_t>& _file, std::string* _error)
	{
		auto u32 = [&](size_t i) {
			return static_cast<uint32_t>(_file[i]) | (_file[i + 1] << 8) | (_file[i + 2] << 16) | (static_cast<uint32_t>(_file[i + 3]) << 24);
		};
		if (_file.size() < 8 || _file.size() % 4) return fail(_error, "size is not a multiple of 4");
		const uint32_t count = u32(0);
		loadAddress = u32(4);
		if (8 + size_t(count) * 4 > _file.size()) return fail(_error, "word count exceeds file size");
		words.resize(count);
		for (uint32_t i = 0; i < count; ++i) words[i] = u32(8 + 4 * i) & 0xFFFFFF;
		return true;
	}

	bool loadAlgorithmDll(const fs::path& _dll, AlgorithmImage& _out, std::string* _error)
	{
		Pe32Image pe;
		std::string err;
		if (!pe.load(_dll, &err)) return fail(_error, err);
		const auto fn = pe.exportVA("GetAlgorithmStruct");
		if (!fn) return fail(_error, "no GetAlgorithmStruct export");
		// GetAlgorithmStruct is "mov eax, imm32; ret"
		uint8_t code[6];
		if (!pe.read(*fn, code, 6) || code[0] != 0xB8 || code[5] != 0xC3) return fail(_error, "unexpected GetAlgorithmStruct code");
		const uint32_t alg = code[1] | (code[2] << 8) | (code[3] << 16) | (static_cast<uint32_t>(code[4]) << 24);

		_out = {};
		_out.dllName = _dll.filename().string();
		_out.algStructVA = alg;
		if (const auto np = pe.u32(alg + AlgNameOffset)) _out.name = pe.cstring(*np);

		const auto speeds = pe.u32(alg + AlgSpeedsOffset);
		if (!speeds) return fail(_error, "no speed table");
		const auto single = pe.u32(*speeds), dbl = pe.u32(*speeds + 4);
		if (!single || *single == 0) return fail(_error, "no single-speed program");
		if (!readUcodeSet(pe, *single, _out.singleSpeed, _error)) return false;
		if (dbl && *dbl && !readUcodeSet(pe, *dbl, _out.doubleSpeed, _error)) return false;
		return true;
	}

	bool Firmware::load(const fs::path& _root, std::string* _error)
	{
		std::error_code ec;
		m_root = fs::exists(_root / "960L", ec) ? _root / "960L" : _root;
		if (!fs::exists(m_root / "Cham", ec) || !fs::exists(m_root / "App", ec))
			return fail(_error, "not a 960L folder (expected Cham/ and App/): " + m_root.string());

		warnings.clear();
		m_algorithms.clear();
		const auto cham = m_root / "Cham";
		if (!loadBootImage(cham, "ChamDSP.pci", m_chamDsp, _error)) return false;
		if (!loadBootImage(cham, "ChamID.pci", m_chamId, _error)) return false;
		const auto z80 = findCI(cham, "Z80OS.PCI");
		if (z80.empty() || !readFile(z80, m_z80os)) return fail(_error, "missing Z80OS.PCI");

		std::vector<fs::path> dlls;
		for (const auto& e : fs::directory_iterator(m_root / "App" / "Algorithms", ec))
			if (lower(e.path().extension().string()) == ".dll") dlls.push_back(e.path());
		std::sort(dlls.begin(), dlls.end());
		for (const auto& d : dlls)
		{
			AlgorithmImage a;
			std::string err;
			if (loadAlgorithmDll(d, a, &err)) m_algorithms.push_back(std::move(a));
			else warnings.push_back(d.filename().string() + ": " + err);
		}
		if (m_algorithms.empty()) return fail(_error, "no algorithm DLLs could be read");
		return true;
	}

	const AlgorithmImage* Firmware::findAlgorithm(const std::string& _name) const
	{
		for (const auto& a : m_algorithms)
			if (a.name == _name || lower(a.dllName) == lower(_name) || lower(a.dllName) == lower(_name + ".dll")) return &a;
		return nullptr;
	}
}
