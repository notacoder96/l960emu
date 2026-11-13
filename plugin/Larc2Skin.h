// Image-based LARC2 front panel ("skin"), loaded at run time from a folder:
//
//   skin.json            layout (see docs/SKINS.md)
//   background.png       the whole remote, drawn at the size given in skin.json
//   any other images     fader caps, joystick knob, pressed-key overlays ... referenced from skin.json
//
// Every control is a rectangle in background-image pixels. The skin scales to the editor, keeping its aspect ratio.
#pragma once

#include "l960/larc2/larc2device.h"

#include <juce_gui_basics/juce_gui_basics.h>

struct Larc2Skin
{
	struct Key { juce::String name; juce::Rectangle<float> r; juce::Image up, down; };
	struct Fader { int index = 0; juce::Rectangle<float> track; juce::Image cap; float capW = 0, capH = 0; };
	struct Joy { juce::Rectangle<float> r; juce::Image knob; float knobSize = 0; bool present = false; };

	juce::File folder;
	juce::Image background;
	float width = 0, height = 0;
	juce::Rectangle<float> lcd;
	juce::Colour pressedTint{0x60ffffff};
	bool showHotspots = false;  // outlines every control: for lining a layout up with a photo
	std::vector<Key> keys;
	std::vector<Fader> faders;
	Joy joystick;

	// Returns an empty string on success, otherwise what is wrong with the skin
	juce::String load(const juce::File& _folder);
};

class SkinnedLarc2Component final : public juce::Component, private juce::Timer
{
public:
	SkinnedLarc2Component(l960::larc2::Larc2Device& _dev, Larc2Skin _skin);
	~SkinnedLarc2Component() override;

	void paint(juce::Graphics&) override;
	void mouseDown(const juce::MouseEvent&) override;
	void mouseDrag(const juce::MouseEvent&) override;
	void mouseUp(const juce::MouseEvent&) override;

	const Larc2Skin& skin() const { return m_skin; }

private:
	void timerCallback() override;
	juce::AffineTransform toScreen() const;
	juce::Point<float> toSkin(juce::Point<float> _p) const;
	void setFaderFrom(size_t _i, juce::Point<float> _skinPos);
	void setJoyFrom(juce::Point<float> _skinPos);

	l960::larc2::Larc2Device& m_dev;
	Larc2Skin m_skin;
	juce::Image m_lcd{juce::Image::RGB, 640, 240, true};
	uint64_t m_frame = ~0ull;

	std::vector<float> m_faderPos;     // 0..1 per skin fader
	juce::Point<float> m_joy{0.5f, 0.5f};
	int m_keyDown = -1, m_faderDrag = -1;
	bool m_joyDrag = false;
};
