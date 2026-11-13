cmake_minimum_required(VERSION 3.22)

project(L960Emu VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

option(L960_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/Standalone)" ON)
option(L960_BUILD_TESTS "Build the core tests" ON)
option(L960_WITH_DSP56300 "Build the DSP56301 core (dsp56300, GPL-3)" ON)

include(cmake/dependencies.cmake)

add_subdirectory(core)

if(L960_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

if(L960_BUILD_PLUGIN)
	add_subdirectory(plugin)
endif()
