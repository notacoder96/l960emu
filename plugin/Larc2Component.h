// On-screen LARC2: the emulated 640x240 LCD, the keypad, 8 faders and the joystick.
#pragma once

#include "l960/larc2/larc2device.h"

#include <juce_gui_basics/juce_gui_basics.h>

class Larc2Component final : public juce::Component, private juce::Timer
{
public:
	explicit Larc2Component(l960::larc2::Larc2Device& _dev);
	~Larc2Component() override;

	void paint(juce::Graphics&) override;
	void resized() override;

private:
	class KeyButton;
	class Joystick;

	void timerCallback() override;

	l960::larc2::Larc2Device& m_dev;
	juce::Image m_lcd{juce::Image::RGB, 640, 240, true};
	uint64_t m_frame = ~0ull;
	juce::Rectangle<int> m_lcdArea;
	juce::OwnedArray<KeyButton> m_softKeys, m_keys;
	juce::OwnedArray<juce::Slider> m_faders;
	std::unique_ptr<Joystick> m_joy;
	juce::Label m_status;
};
