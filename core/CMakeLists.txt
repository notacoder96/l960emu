add_library(l960core STATIC
	include/l960/pe32.h        src/pe32.cpp
	include/l960/firmware.h    src/firmware.cpp
	include/l960/lexichip.h    src/lexichip.cpp
	include/l960/z80unit.h     src/z80unit.cpp
	include/l960/dsp56301.h    src/dsp56301.cpp
	include/l960/card.h        src/card.cpp
	include/l960/chamdevice.h  src/chamdevice.cpp
	include/l960/engine.h      src/engine.cpp
	include/l960/hostbridge.h  src/hostbridge.cpp
	include/l960/hostsystem.h  src/hostsystem.cpp
	${CMAKE_CURRENT_BINARY_DIR}/hookdll_data.cpp
	include/l960/larc2/arm4.h  src/larc2/arm4.cpp
	include/l960/larc2/larc2.h src/larc2/larc2.cpp
	include/l960/larc2/larc2device.h src/larc2/larc2device.cpp
	src/chips_impl.c)

# The 32-bit hook DLL for the helper process (hosthelper/, prebuilt with MinGW) is embedded so the plugin can
# install it into its private copy of the 960L software.
set(L960_HOOK_DLL ${CMAKE_SOURCE_DIR}/hosthelper/Enums.dll CACHE FILEPATH "32-bit hook DLL embedded for the helper process")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hookdll_data.cpp
	COMMAND ${CMAKE_COMMAND} -DINPUT=${L960_HOOK_DLL} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/hookdll_data.cpp -DSYMBOL=kL960HookDll
	        -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS ${L960_HOOK_DLL} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
	VERBATIM)

target_include_directories(l960core PUBLIC include PRIVATE ${L960_CHIPS_INCLUDE})

find_package(Threads REQUIRED)
target_link_libraries(l960core PUBLIC Threads::Threads)
if(WIN32)
	target_link_libraries(l960core PUBLIC ws2_32)
endif()

if(L960_WITH_DSP56300)
	target_link_libraries(l960core PRIVATE dsp56kEmu)
	target_compile_definitions(l960core PRIVATE L960_HAVE_DSP56300=1)
endif()

if(MSVC)
	target_compile_options(l960core PRIVATE /W4)
else()
	target_compile_options(l960core PRIVATE -Wall -Wextra $<$<COMPILE_LANGUAGE:C>:-Wno-unused-parameter>)
endif()
