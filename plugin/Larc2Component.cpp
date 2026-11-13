#include "Larc2Component.h"

namespace
{
	const juce::Colour Panel(0xff2a2c30), KeyFace(0xff3a3d42), KeyText(0xffe8e8e8), Accent(0xffd8b04a);
}

// Momentary key: down on mouse press, up on release
class Larc2Component::KeyButton final : public juce::Component
{
public:
	KeyButton(l960::larc2::Larc2Device& _dev, juce::String _key, juce::String _label) : m_dev(_dev), m_key(std::move(_key)), m_label(std::move(_label)) {}

	void paint(juce::Graphics& g) override
	{
		auto r = getLocalBounds().toFloat().reduced(2.0f);
		g.setColour(m_down ? Accent : KeyFace);
		g.fillRoundedRectangle(r, 4.0f);
		g.setColour(juce::Colours::black.withAlpha(0.6f));
		g.drawRoundedRectangle(r, 4.0f, 1.0f);
		g.setColour(m_down ? juce::Colours::black : KeyText);
		g.setFont(juce::FontOptions(m_label.length() > 6 ? 10.0f : 12.0f, juce::Font::bold));
		g.drawFittedText(m_label, getLocalBounds().reduced(3), juce::Justification::centred, 2);
	}

	void mouseDown(const juce::MouseEvent&) override { m_down = true; m_dev.pressKey(m_key.toStdString(), true); repaint(); }
	void mouseUp(const juce::MouseEvent&) override { m_down = false; m_dev.pressKey(m_key.toStdString(), false); repaint(); }

private:
	l960::larc2::Larc2Device& m_dev;
	juce::String m_key, m_label;
	bool m_down = false;
};

// Spring-loaded joystick: drag to move, release to recentre
class Larc2Component::Joystick final : public juce::Component
{
public:
	explicit Joystick(l960::larc2::Larc2Device& _dev) : m_dev(_dev) {}

	void paint(juce::Graphics& g) override
	{
		auto r = getLocalBounds().toFloat().reduced(2.0f);
		g.setColour(KeyFace);
		g.fillEllipse(r);
		g.setColour(juce::Colours::black.withAlpha(0.5f));
		g.drawEllipse(r, 1.0f);
		const auto c = juce::Point<float>(r.getX() + m_pos.x * r.getWidth(), r.getY() + (1.0f - m_pos.y) * r.getHeight());
		g.setColour(Accent);
		g.fillEllipse(juce::Rectangle<float>(18.0f, 18.0f).withCentre(c));
	}

	void mouseDrag(const juce::MouseEvent& e) override
	{
		const auto r = getLocalBounds().toFloat();
		m_pos = {juce::jlimit(0.0f, 1.0f, e.position.x / r.getWidth()), juce::jlimit(0.0f, 1.0f, 1.0f - e.position.y / r.getHeight())};
		m_dev.setJoystick(m_pos.x, m_pos.y);
		repaint();
	}
	void mouseDown(const juce::MouseEvent& e) override { mouseDrag(e); }
	// The LARC2 joystick stays where it is left (no spring), so releasing the mouse keeps the position
	void mouseUp(const juce::MouseEvent&) override {}

private:
	l960::larc2::Larc2Device& m_dev;
	juce::Point<float> m_pos{0.5f, 0.5f};
};

Larc2Component::Larc2Component(l960::larc2::Larc2Device& _dev) : m_dev(_dev)
{
	for (int i = 0; i < 8; ++i)
	{
		auto* k = m_softKeys.add(new KeyButton(m_dev, "F" + juce::String(i + 1), "F" + juce::String(i + 1)));
		addAndMakeVisible(k);
		auto* f = m_faders.add(new juce::Slider(juce::Slider::LinearVertical, juce::Slider::NoTextBox));
		f->setRange(0.0, 1.0);
		f->setValue(0.5, juce::dontSendNotification);
		f->onValueChange = [this, i, f] { m_dev.setFader(i, float(f->getValue())); };
		// The LARC2 faders are touch-sensitive: the remote only reports moves while a fader is touched
		f->onDragStart = [this, i] { m_dev.setFaderTouched(i, true); };
		f->onDragEnd = [this, i] { m_dev.setFaderTouched(i, false); };
		addAndMakeVisible(f);
	}

	// Keypad, laid out roughly like the LARC2 front panel (names from the boot ROM key table)
	const char* const keys[] = {
		"PROGRAM", "REGISTER", "BANK", "STORE", "EDIT", "CONTROL", "MACHINE", "JOYSTICK",
		"1", "2", "3", "Up", "MUTE MACH", "MUTE ALL", "FINE ADJUST", "LEXICON",
		"4", "5", "6", "Left", "Down", "Right", "-", "+",
		"7", "8", "9", "0", "ENTER"};
	for (const char* k : keys)
	{
		auto* b = m_keys.add(new KeyButton(m_dev, k, k));
		addAndMakeVisible(b);
	}

	m_joy = std::make_unique<Joystick>(m_dev);
	addAndMakeVisible(*m_joy);

	m_status.setFont(juce::FontOptions(12.0f));
	m_status.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
	addAndMakeVisible(m_status);

	startTimerHz(30);
}

Larc2Component::~Larc2Component() { stopTimer(); }

void Larc2Component::timerCallback()
{
	std::vector<uint8_t> rgb;
	uint32_t w, h;
	const uint64_t f = m_dev.lcd(rgb, w, h);
	m_status.setText("LARC2: " + juce::String(m_dev.status()) + "   (" + juce::String(double(m_dev.instructions()) / 1e6, 0) + "M instructions)",
	                 juce::dontSendNotification);
	if (f == m_frame || rgb.size() < size_t(w) * h * 3) return;
	m_frame = f;
	if (m_lcd.getWidth() != int(w) || m_lcd.getHeight() != int(h)) m_lcd = juce::Image(juce::Image::RGB, int(w), int(h), true);
	juce::Image::BitmapData bd(m_lcd, juce::Image::BitmapData::writeOnly);
	for (uint32_t y = 0; y < h; ++y)
		for (uint32_t x = 0; x < w; ++x)
		{
			const uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
			bd.setPixelColour(int(x), int(y), juce::Colour(p[0], p[1], p[2]));
		}
	repaint(m_lcdArea);
}

void Larc2Component::paint(juce::Graphics& g)
{
	g.fillAll(Panel);
	g.setColour(juce::Colours::black);
	g.fillRect(m_lcdArea.expanded(6));
	g.drawImage(m_lcd, m_lcdArea.toFloat(), juce::RectanglePlacement::stretchToFit);
}

void Larc2Component::resized()
{
	auto r = getLocalBounds().reduced(10);
	m_status.setBounds(r.removeFromBottom(18));

	auto right = r.removeFromRight(330);
	auto left = r;

	// LCD at native 640 x 240 (scaled down if needed), soft keys under it, faders under those
	const int lcdW = juce::jmin(left.getWidth() - 12, 640);
	m_lcdArea = juce::Rectangle<int>(left.getX() + 6, left.getY() + 6, lcdW, lcdW * 240 / 640);
	auto below = left.withTop(m_lcdArea.getBottom() + 12);
	auto softRow = below.removeFromTop(32).withWidth(lcdW).withX(m_lcdArea.getX());
	const int sw = softRow.getWidth() / 8;
	for (int i = 0; i < 8; ++i) m_softKeys[i]->setBounds(softRow.removeFromLeft(sw));
	auto faderRow = below.withHeight(juce::jmin(below.getHeight(), 170)).withWidth(lcdW).withX(m_lcdArea.getX());
	for (int i = 0; i < 8; ++i) m_faders[i]->setBounds(faderRow.removeFromLeft(sw).reduced(sw / 4, 4));

	// Keypad grid (8 columns), joystick below
	auto pad = right.removeFromTop(4 * 44);
	const int kw = pad.getWidth() / 8, kh = 44;
	for (int i = 0; i < m_keys.size(); ++i)
	{
		const int row = i / 8, col = i % 8;
		int span = 1;
		if (i == m_keys.size() - 1) span = 4;  // ENTER spans the rest of the last row
		m_keys[i]->setBounds(pad.getX() + col * kw, pad.getY() + row * kh, kw * span, kh);
	}
	right.removeFromTop(10);
	const int js = juce::jmin(right.getWidth(), right.getHeight(), 150);
	m_joy->setBounds(right.getX() + (right.getWidth() - js) / 2, right.getY(), js, js);
}
