#pragma once

#include "Larc2Component.h"
#include "Larc2Skin.h"
#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

class L960AudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
	explicit L960AudioProcessorEditor(L960AudioProcessor&);

	void paint(juce::Graphics&) override;
	void resized() override;

private:
	void chooseFirmware();
	void refresh();
	void timerCallback() override;
	void chooseSkin();
	void applySkin();  // builds the remote from the processor's skin folder (built-in look when none)

	L960AudioProcessor& m_proc;
	juce::TextButton m_locate{"Locate 960L folder..."};
	juce::ComboBox m_outPair;
	juce::TextButton m_restart{"Restart 960L"};
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> m_outPairA;
	juce::Label m_status;
	juce::Slider m_mix, m_in, m_out;
	juce::Label m_mixLabel{{}, "Wet/Dry"}, m_inLabel{{}, "Input"}, m_outLabel{{}, "Output"};
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_mixA, m_inA, m_outA;
	std::unique_ptr<juce::FileChooser> m_chooser;
	juce::TextButton m_skinButton{"Skin..."};
	std::unique_ptr<juce::Component> m_remote;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(L960AudioProcessorEditor)
};
