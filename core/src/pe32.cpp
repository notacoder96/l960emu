#include "l960/pe32.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace l960
{
	namespace
	{
		template<typename T> bool rd(const std::vector<uint8_t>& _f, size_t _off, T& _out)
		{
			if (_off + sizeof(T) > _f.size()) return false;
			std::memcpy(&_out, _f.data() + _off, sizeof(T));
			return true;
		}

		bool fail(std::string* _e, const char* _msg)
		{
			if (_e) *_e = _msg;
			return false;
		}
	}

	bool Pe32Image::load(const std::filesystem::path& _file, std::string* _error)
	{
		std::ifstream f(_file, std::ios::binary);
		if (!f) return fail(_error, "cannot open file");
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		return loadFromMemory(std::move(data), _error);
	}

	bool Pe32Image::loadFromMemory(std::vector<uint8_t> _data, std::string* _error)
	{
		m_file = std::move(_data);
		m_sections.clear();
		m_exports.clear();
		return parse(_error);
	}

	bool Pe32Image::parse(std::string* _error)
	{
		uint16_t mz = 0;
		if (!rd(m_file, 0, mz) || mz != 0x5A4D) return fail(_error, "not an MZ executable");
		uint32_t peOff = 0;
		if (!rd(m_file, 0x3C, peOff)) return fail(_error, "truncated DOS header");
		uint32_t sig = 0;
		if (!rd(m_file, peOff, sig) || sig != 0x00004550) return fail(_error, "missing PE signature");

		uint16_t machine = 0, numSections = 0, optSize = 0;
		rd(m_file, peOff + 4, machine);
		rd(m_file, peOff + 6, numSections);
		rd(m_file, peOff + 20, optSize);
		if (machine != 0x14C) return fail(_error, "not an i386 image");

		const size_t opt = peOff + 24;
		uint16_t magic = 0;
		rd(m_file, opt, magic);
		if (magic != 0x10B) return fail(_error, "not a PE32 image");
		rd(m_file, opt + 28, m_imageBase);

		uint32_t exportRva = 0, exportSize = 0;
		rd(m_file, opt + 96, exportRva);
		rd(m_file, opt + 100, exportSize);

		const size_t secTable = opt + optSize;
		for (uint16_t i = 0; i < numSections; ++i)
		{
			const size_t s = secTable + i * 40;
			Section sec{};
			if (!rd(m_file, s + 8, sec.vsize) || !rd(m_file, s + 12, sec.va) ||
				!rd(m_file, s + 16, sec.rawSize) || !rd(m_file, s + 20, sec.rawOffset))
				return fail(_error, "truncated section table");
			m_sections.push_back(sec);
		}

		if (exportRva)
		{
			const uint32_t ed = m_imageBase + exportRva;
			const auto numNames = u32(ed + 24), funcs = u32(ed + 28), names = u32(ed + 32), ords = u32(ed + 36);
			if (!numNames || !funcs || !names || !ords) return fail(_error, "bad export directory");
			for (uint32_t i = 0; i < *numNames; ++i)
			{
				const auto nameRva = u32(m_imageBase + *names + 4 * i);
				const auto ord = u16(m_imageBase + *ords + 2 * i);
				if (!nameRva || !ord) continue;
				const auto fnRva = u32(m_imageBase + *funcs + 4 * *ord);
				if (!fnRva) continue;
				m_exports[cstring(m_imageBase + *nameRva)] = m_imageBase + *fnRva;
			}
		}
		return true;
	}

	const Pe32Image::Section* Pe32Image::findSection(const uint32_t _rva) const
	{
		for (const auto& s : m_sections)
		{
			const uint32_t size = std::max(s.vsize, s.rawSize);
			if (_rva >= s.va && _rva < s.va + size) return &s;
		}
		return nullptr;
	}

	bool Pe32Image::read(const uint32_t _va, void* _dst, const size_t _size) const
	{
		if (_va < m_imageBase) return false;
		auto* dst = static_cast<uint8_t*>(_dst);
		for (size_t i = 0; i < _size; ++i)
		{
			const uint32_t rva = _va - m_imageBase + static_cast<uint32_t>(i);
			const auto* s = findSection(rva);
			if (!s) return false;
			const uint32_t off = rva - s->va;
			// bytes past the raw data but inside the virtual size are zero-initialised (.bss)
			dst[i] = off < s->rawSize && s->rawOffset + off < m_file.size() ? m_file[s->rawOffset + off] : 0;
		}
		return true;
	}

	std::optional<uint32_t> Pe32Image::u32(const uint32_t _va) const
	{
		uint32_t v;
		if (!read(_va, &v, 4)) return std::nullopt;
		return v;
	}

	std::optional<uint16_t> Pe32Image::u16(const uint32_t _va) const
	{
		uint16_t v;
		if (!read(_va, &v, 2)) return std::nullopt;
		return v;
	}

	std::optional<uint8_t> Pe32Image::u8(const uint32_t _va) const
	{
		uint8_t v;
		if (!read(_va, &v, 1)) return std::nullopt;
		return v;
	}

	std::string Pe32Image::cstring(const uint32_t _va, const size_t _maxLen) const
	{
		std::string s;
		for (size_t i = 0; i < _maxLen; ++i)
		{
			const auto c = u8(_va + static_cast<uint32_t>(i));
			if (!c || *c == 0) break;
			s.push_back(static_cast<char>(*c));
		}
		return s;
	}

	std::optional<uint32_t> Pe32Image::exportVA(const std::string& _name) const
	{
		const auto it = m_exports.find(_name);
		if (it == m_exports.end()) return std::nullopt;
		return it->second;
	}
}
