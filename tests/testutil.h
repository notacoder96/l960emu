#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define L960_CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

// Tests need the extracted 960L software folder (not distributed). Set L960_FIRMWARE_DIR or pass it as argv[1].
inline std::filesystem::path firmwareDir(int argc, char** argv)
{
	if (argc > 1) return argv[1];
	if (const char* e = std::getenv("L960_FIRMWARE_DIR")) return e;
	std::fprintf(stderr, "SKIP: set L960_FIRMWARE_DIR to the extracted 960L folder\n");
	std::exit(77);
}
