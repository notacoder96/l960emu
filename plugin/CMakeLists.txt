juce_add_plugin(L960Emu
	COMPANY_NAME "notacoder"
	BUNDLE_ID com.notacoder.l960emu
	PRODUCT_NAME "960L Emulator"
	PLUGIN_MANUFACTURER_CODE Ntcd
	PLUGIN_CODE L960
	FORMATS VST3 AU Standalone
	IS_SYNTH FALSE
	NEEDS_MIDI_INPUT FALSE
	NEEDS_MIDI_OUTPUT FALSE
	COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(L960Emu PRIVATE
	PluginProcessor.cpp PluginProcessor.h
	PluginEditor.cpp PluginEditor.h
	Larc2Component.cpp Larc2Component.h
	Larc2Skin.cpp Larc2Skin.h)

target_compile_definitions(L960Emu PUBLIC
	JUCE_WEB_BROWSER=0
	JUCE_USE_CURL=0
	JUCE_VST3_CAN_REPLACE_VST2=0
	JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(L960Emu
	PRIVATE l960core juce::juce_audio_utils juce::juce_dsp
	PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Headless smoke test of the processor (no host, no GUI): cmake -DL960_BUILD_PLUGIN_SMOKE=ON
option(L960_BUILD_PLUGIN_SMOKE "Build a headless processor smoke test" OFF)
if(L960_BUILD_PLUGIN_SMOKE)
	juce_add_console_app(plugin_smoke PRODUCT_NAME "plugin_smoke")
	target_sources(plugin_smoke PRIVATE plugin_smoke.cpp PluginProcessor.cpp PluginEditor.cpp Larc2Component.cpp Larc2Skin.cpp)
	target_compile_definitions(plugin_smoke PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
	target_link_libraries(plugin_smoke PRIVATE l960core juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()
