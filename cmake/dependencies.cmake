include(FetchContent)

# Pinned third-party revisions
set(L960_CHIPS_COMMIT    9e88298ce56319953ac7a43213a1120359f7a3a6)   # floooh/chips, 2026-09-26, zlib
set(L960_DSP56300_COMMIT 46aa691da84a674fe843d406d36c89c7efcc8c01)   # dsp56300/dsp56300, 2026-09-15, GPL-3
set(L960_JUCE_TAG        8.0.4)                                      # juce-framework/JUCE

# Z80 core (header only)
FetchContent_Declare(chips
	GIT_REPOSITORY https://github.com/floooh/chips.git
	GIT_TAG        ${L960_CHIPS_COMMIT}
	SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(chips)
set(L960_CHIPS_INCLUDE ${chips_SOURCE_DIR})

# DSP56300 family core, patched to accept host-defined peripherals (the 56301 has HI32, not HI08)
if(L960_WITH_DSP56300)
	find_package(Git REQUIRED)
	FetchContent_Declare(dsp56300
		GIT_REPOSITORY https://github.com/dsp56300/dsp56300.git
		GIT_TAG        ${L960_DSP56300_COMMIT}
		GIT_SUBMODULES source/asmjit
		UPDATE_DISCONNECTED TRUE
		SOURCE_SUBDIR  do-not-build)
	FetchContent_MakeAvailable(dsp56300)

	# Apply our patch, and re-apply it whenever patches/dsp56300-generic-peripherals.patch changes (FetchContent's
	# PATCH_COMMAND only runs on the first download, which left existing build trees on an old patch).
	set(_patch ${CMAKE_CURRENT_LIST_DIR}/../patches/dsp56300-generic-peripherals.patch)
	file(MD5 ${_patch} _patch_md5)
	set(_marker ${dsp56300_SOURCE_DIR}/.l960_patch_md5)
	set(_applied "")
	if(EXISTS ${_marker})
		file(READ ${_marker} _applied)
	endif()
	if(NOT _applied STREQUAL _patch_md5)
		message(STATUS "Applying ${_patch}")
		execute_process(COMMAND ${GIT_EXECUTABLE} checkout -- . WORKING_DIRECTORY ${dsp56300_SOURCE_DIR})
		execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${_patch} WORKING_DIRECTORY ${dsp56300_SOURCE_DIR}
		                RESULT_VARIABLE _rc)
		if(NOT _rc EQUAL 0)
			message(FATAL_ERROR "Could not apply ${_patch} to ${dsp56300_SOURCE_DIR}")
		endif()
		file(WRITE ${_marker} ${_patch_md5})
	endif()

	set(ASMJIT_STATIC TRUE CACHE BOOL "" FORCE)
	set(DSP56300_DEBUGGER OFF CACHE BOOL "" FORCE)
	add_subdirectory(${dsp56300_SOURCE_DIR}/source ${CMAKE_BINARY_DIR}/dsp56300 EXCLUDE_FROM_ALL)
endif()

if(L960_BUILD_PLUGIN)
	FetchContent_Declare(juce
		GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
		GIT_TAG        ${L960_JUCE_TAG}
		GIT_SHALLOW    TRUE)
	FetchContent_MakeAvailable(juce)
endif()
