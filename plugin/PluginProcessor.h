#pragma once

#include "l960/engine.h"
#include "l960/hostsystem.h"
#include "l960/larc2/larc2device.h"

#include <juce_audio_processors/juce_audio_processors.h>

// The plugin runs the 960L as a whole: the emulated reverb card and LARC2 here, the original host software as a
// helper process (see l960::HostSystem). The LARC2 on screen is the user interface, exactly as on the hardware.
class L960AudioProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
	L960AudioProcessor();
	~L960AudioProcessor() override;

	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override {}
	bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
	void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "960L Emulator"; }
	bool acceptsMidi() const override { return false; }
	bool producesMidi() const override { return false; }
	double getTailLengthSeconds() const override { return 10.0; }

	int getNumPrograms() override { return 1; }
	int getCurrentProgram() override { return 0; }
	void setCurrentProgram(int) override {}
	const juce::String getProgramName(int) override { return {}; }
	void changeProgramName(int, const juce::String&) override {}

	void getStateInformation(juce::MemoryBlock& destData) override;
	void setStateInformation(const void* data, int sizeInBytes) override;

	// message thread
	bool loadFirmware(const juce::File& folder, juce::String& error);
	juce::File firmwareFolder() const { return m_firmwareFolder; }
	juce::String statusText() const;
	void restartHost();
	juce::String skinFolder() const { return m_skinFolder; }
	void setSkinFolder(const juce::String& _f) { m_skinFolder = _f; }

	juce::AudioProcessorValueTreeState params;
	l960::larc2::Larc2Device& larc2() { return m_larc2; }

private:
	static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
	void timerCallback() override;
	static juce::File dataFolder();

	l960::Engine m_engine;
	l960::larc2::Larc2Device m_larc2;
	std::unique_ptr<l960::HostSystem> m_host;
	juce::SpinLock m_engineLock;  // held by the audio thread while processing; message thread (re)loads firmware
	juce::File m_firmwareFolder;
	juce::String m_hostError;
	juce::String m_skinFolder;
	bool m_helperWanted = false;
	double m_sampleRate = 48000.0;
	std::atomic<float>* m_outPair = nullptr;

	juce::AudioBuffer<float> m_wet;
	std::atomic<float>* m_mix = nullptr;
	std::atomic<float>* m_inGain = nullptr;
	std::atomic<float>* m_outGain = nullptr;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(L960AudioProcessor)
};
