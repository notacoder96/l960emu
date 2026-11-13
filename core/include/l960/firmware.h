// Loads the pieces of a 960L software release (the "960L" folder from the V4.x CD/ISO) that the
// emulator needs: the reverb-card firmware images and the Lexichip programs inside the algorithm DLLs.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace l960
{
	// A DSP56301 load image as sent over PCI by ChamAPI (Cham/*.pci for the 56K):
	// little-endian u32 stream {wordCount, loadAddress, word[wordCount]}, 24 bits used per word.
	struct Dsp56kBootImage
	{
		uint32_t loadAddress = 0;
		std::vector<uint32_t> words;

		bool parse(const std::vector<uint8_t>& _file, std::string* _error = nullptr);
	};

	// A Lexichip III program as described by the UCODE struct that ChamAPI::LoadLexichip consumes.
	struct Ucode
	{
		static constexpr size_t PlaneCount = 5;

		uint16_t lines = 0;                                   // 256 at 44.1/48 kHz, 128 at 88.2/96 kHz
		std::array<std::vector<uint8_t>, PlaneCount> wcs;     // byte-planar writable control store
		std::vector<uint32_t> xmoq;                           // external memory operation queue entries

		// WCS line i as {b0..b4}
		std::array<uint8_t, PlaneCount> line(size_t _i) const
		{
			return {wcs[0][_i], wcs[1][_i], wcs[2][_i], wcs[3][_i], wcs[4][_i]};
		}
	};

	struct AlgorithmImage
	{
		std::string dllName;       // e.g. "Plate.dll"
		std::string name;          // e.g. "Plate"
		uint32_t algStructVA = 0;  // ALG_INIT_STRUCT address inside the DLL
		std::vector<Ucode> singleSpeed;  // one Ucode per Lexichip used
		std::vector<Ucode> doubleSpeed;  // empty when the algorithm has no 96 kHz version
	};

	// Extracts the Lexichip programs from one algorithm DLL.
	bool loadAlgorithmDll(const std::filesystem::path& _dll, AlgorithmImage& _out, std::string* _error = nullptr);

	class Firmware
	{
	public:
		// _root may be the extracted CD root (containing "960L") or the "960L" folder itself.
		bool load(const std::filesystem::path& _root, std::string* _error = nullptr);

		const Dsp56kBootImage& chamDsp() const { return m_chamDsp; }   // Cham/ChamDSP.pci, main 56301 program
		const Dsp56kBootImage& chamId() const { return m_chamId; }     // Cham/ChamID.pci, startup/ID program
		const std::vector<uint8_t>& z80Kernel() const { return m_z80os; } // Cham/Z80OS.PCI, loaded into DPRAM
		const std::vector<AlgorithmImage>& algorithms() const { return m_algorithms; }
		const AlgorithmImage* findAlgorithm(const std::string& _name) const;
		const std::filesystem::path& root() const { return m_root; }
		std::filesystem::path factoryPresetDir() const { return m_root / "FactoryPresets"; }

		std::vector<std::string> warnings;

	private:
		std::filesystem::path m_root;
		Dsp56kBootImage m_chamDsp, m_chamId;
		std::vector<uint8_t> m_z80os;
		std::vector<AlgorithmImage> m_algorithms;
	};

	bool readFile(const std::filesystem::path& _file, std::vector<uint8_t>& _out);
}
