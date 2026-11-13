// Minimal read-only PE32 (Win32 DLL) image reader.
// Used to pull Lexichip microcode and algorithm tables straight out of the 960L algorithm DLLs
// without loading or executing them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace l960
{
	class Pe32Image
	{
	public:
		bool load(const std::filesystem::path& _file, std::string* _error = nullptr);
		bool loadFromMemory(std::vector<uint8_t> _data, std::string* _error = nullptr);

		uint32_t imageBase() const { return m_imageBase; }

		// Virtual address (absolute, image base included) accessors. Return false/nullopt when the
		// range is not backed by a section's raw data.
		bool read(uint32_t _va, void* _dst, size_t _size) const;
		std::optional<uint32_t> u32(uint32_t _va) const;
		std::optional<uint16_t> u16(uint32_t _va) const;
		std::optional<uint8_t> u8(uint32_t _va) const;
		std::string cstring(uint32_t _va, size_t _maxLen = 256) const;

		// Exported symbol name -> absolute VA
		const std::map<std::string, uint32_t>& exports() const { return m_exports; }
		std::optional<uint32_t> exportVA(const std::string& _name) const;

	private:
		struct Section { uint32_t va; uint32_t vsize; uint32_t rawOffset; uint32_t rawSize; };
		const Section* findSection(uint32_t _rva) const;
		bool parse(std::string* _error);

		std::vector<uint8_t> m_file;
		std::vector<Section> m_sections;
		std::map<std::string, uint32_t> m_exports;
		uint32_t m_imageBase = 0;
	};
}
