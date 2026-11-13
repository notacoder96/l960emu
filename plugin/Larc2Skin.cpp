#include "Larc2Skin.h"

namespace
{
	juce::Rectangle<float> rectOf(const juce::var& _v)
	{
		if (auto* a = _v.getArray(); a && a->size() == 4)
			return {float(double((*a)[0])), float(double((*a)[1])), float(double((*a)[2])), float(double((*a)[3]))};
		return {float(double(_v["x"])), float(double(_v["y"])), float(double(_v["w"])), float(double(_v["h"]))};
	}

	juce::Image imageOf(const juce::File& _folder, const juce::var& _name)
	{
		if (!_name.isString() || _name.toString().isEmpty()) return {};
		return juce::ImageFileFormat::loadFrom(_folder.getChildFile(_name.toString()));
	}

	bool isKeyName(const juce::String& _n)
	{
		for (const auto& row : l960::larc2::Larc2Device::KeyNames)
			for (const char* k : row)
				if (*k && _n == k) return true;
		return false;
	}
}

juce::String Larc2Skin::load(const juce::File& _folder)
{
	folder = _folder;
	const auto file = _folder.getChildFile("skin.json");
	if (!file.existsAsFile()) return "no skin.json in " + _folder.getFullPathName();
	juce::var root;
	if (const auto r = juce::JSON::parse(file.loadFileAsString(), root); r.failed()) return "skin.json: " + r.getErrorMessage();

	background = imageOf(_folder, root["background"]);
	width = float(double(root["width"]));
	height = float(double(root["height"]));
	if (background.isValid())
	{
		if (width <= 0) width = float(background.getWidth());
		if (height <= 0) height = float(background.getHeight());
	}
	if (width <= 0 || height <= 0) return "skin.json needs a background image or width/height";

	lcd = rectOf(root["lcd"]);
	if (lcd.isEmpty()) return "skin.json: \"lcd\" rectangle missing";
	showHotspots = bool(root["showHotspots"]);
	if (root.hasProperty("pressedTint")) pressedTint = juce::Colour::fromString(root["pressedTint"].toString());

	keys.clear();
	if (auto* a = root["keys"].getArray())
		for (const auto& k : *a)
		{
			Key key{k["name"].toString(), rectOf(k["rect"]), imageOf(_folder, k["up"]), imageOf(_folder, k["down"])};
			if (!isKeyName(key.name)) return "skin.json: unknown key \"" + key.name + "\"";
			keys.push_back(std::move(key));
		}

	faders.clear();
	const auto defaultCap = imageOf(_folder, root["faderCap"]);
	if (auto* a = root["faders"].getArray())
		for (const auto& f : *a)
		{
			Fader fd;
			fd.index = int(f["index"]);
			fd.track = rectOf(f["track"]);
			fd.cap = f.hasProperty("cap") ? imageOf(_folder, f["cap"]) : defaultCap;
			fd.capW = f.hasProperty("capWidth") ? float(double(f["capWidth"])) : fd.cap.isValid() ? float(fd.cap.getWidth()) : fd.track.getWidth();
			fd.capH = f.hasProperty("capHeight") ? float(double(f["capHeight"])) : fd.cap.isValid() ? float(fd.cap.getHeight()) : 12.0f;
			if (fd.index < 0 || fd.index > 7) return "skin.json: fader index must be 0-7";
			faders.push_back(fd);
		}

	joystick = {};
	if (root.hasProperty("joystick"))
	{
		const auto j = root["joystick"];
		joystick.present = true;
		joystick.r = rectOf(j["rect"]);
		joystick.knob = imageOf(_folder, j["knob"]);
		joystick.knobSize = j.hasProperty("knobSize") ? float(double(j["knobSize"])) : joystick.knob.isValid() ? float(joystick.knob.getWidth()) : 20.0f;
	}
	return {};
}

SkinnedLarc2Component::SkinnedLarc2Component(l960::larc2::Larc2Device& _dev, Larc2Skin _skin) : m_dev(_dev), m_skin(std::move(_skin))
{
	m_faderPos.assign(m_skin.faders.size(), 0.5f);
	startTimerHz(30);
}

SkinnedLarc2Component::~SkinnedLarc2Component() { stopTimer(); }

juce::AffineTransform SkinnedLarc2Component::toScreen() const
{
	const float s = juce::jmin(getWidth() / m_skin.width, getHeight() / m_skin.height);
	const float ox = (getWidth() - m_skin.width * s) * 0.5f, oy = (getHeight() - m_skin.height * s) * 0.5f;
	return juce::AffineTransform::scale(s).translated(ox, oy);
}

juce::Point<float> SkinnedLarc2Component::toSkin(const juce::Point<float> _p) const
{
	return _p.transformedBy(toScreen().inverted());
}

void SkinnedLarc2Component::timerCallback()
{
	std::vector<uint8_t> rgb;
	uint32_t w, h;
	const uint64_t f = m_dev.lcd(rgb, w, h);
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
	repaint(m_skin.lcd.transformedBy(toScreen()).getSmallestIntegerContainer());
}

void SkinnedLarc2Component::paint(juce::Graphics& g)
{
	g.fillAll(juce::Colours::black);
	g.saveState();
	g.addTransform(toScreen());

	const juce::Rectangle<float> all(0, 0, m_skin.width, m_skin.height);
	if (m_skin.background.isValid()) g.drawImage(m_skin.background, all, juce::RectanglePlacement::stretchToFit);
	else { g.setColour(juce::Colour(0xff2a2c30)); g.fillRect(all); }

	g.drawImage(m_lcd, m_skin.lcd, juce::RectanglePlacement::stretchToFit);

	for (size_t i = 0; i < m_skin.keys.size(); ++i)
	{
		const auto& k = m_skin.keys[i];
		const bool down = int(i) == m_keyDown;
		if (down && k.down.isValid()) g.drawImage(k.down, k.r, juce::RectanglePlacement::stretchToFit);
		else if (!down && k.up.isValid()) g.drawImage(k.up, k.r, juce::RectanglePlacement::stretchToFit);
		else if (down) { g.setColour(m_skin.pressedTint); g.fillRoundedRectangle(k.r, 3.0f); }
	}

	for (size_t i = 0; i < m_skin.faders.size(); ++i)
	{
		const auto& f = m_skin.faders[i];
		const float travel = f.track.getHeight() - f.capH;
		const float y = f.track.getY() + (1.0f - m_faderPos[i]) * travel;
		const juce::Rectangle<float> cap(f.track.getCentreX() - f.capW * 0.5f, y, f.capW, f.capH);
		if (f.cap.isValid()) g.drawImage(f.cap, cap, juce::RectanglePlacement::stretchToFit);
		else
		{
			g.setColour(juce::Colour(0xffd0d0d0));
			g.fillRoundedRectangle(cap, 2.0f);
			g.setColour(juce::Colours::black);
			g.drawHorizontalLine(int(cap.getCentreY()), cap.getX() + 2, cap.getRight() - 2);
		}
	}

	if (m_skin.joystick.present)
	{
		const auto& j = m_skin.joystick;
		const juce::Point<float> c(j.r.getX() + m_joy.x * j.r.getWidth(), j.r.getY() + (1.0f - m_joy.y) * j.r.getHeight());
		const auto knob = juce::Rectangle<float>(j.knobSize, j.knobSize).withCentre(c);
		if (j.knob.isValid()) g.drawImage(j.knob, knob, juce::RectanglePlacement::stretchToFit);
		else { g.setColour(juce::Colour(0xffd8b04a)); g.fillEllipse(knob); }
	}
	if (m_skin.showHotspots)
	{
		g.setColour(juce::Colours::magenta);
		g.drawRect(m_skin.lcd, 2.0f);
		for (const auto& k : m_skin.keys)
		{
			g.drawRect(k.r, 1.5f);
			g.setFont(juce::FontOptions(11.0f));
			g.drawText(k.name, k.r.withHeight(12.0f).translated(0, -12.0f), juce::Justification::centredLeft);
		}
		g.setColour(juce::Colours::cyan);
		for (const auto& f : m_skin.faders) g.drawRect(f.track, 1.5f);
		if (m_skin.joystick.present) g.drawRect(m_skin.joystick.r, 1.5f);
	}
	g.restoreState();
}

void SkinnedLarc2Component::setFaderFrom(const size_t _i, const juce::Point<float> _p)
{
	const auto& f = m_skin.faders[_i];
	const float travel = juce::jmax(1.0f, f.track.getHeight() - f.capH);
	const float v = juce::jlimit(0.0f, 1.0f, 1.0f - (_p.y - f.track.getY() - f.capH * 0.5f) / travel);
	m_faderPos[_i] = v;
	m_dev.setFader(f.index, v);
}

void SkinnedLarc2Component::setJoyFrom(const juce::Point<float> _p)
{
	const auto& r = m_skin.joystick.r;
	m_joy = {juce::jlimit(0.0f, 1.0f, (_p.x - r.getX()) / r.getWidth()), juce::jlimit(0.0f, 1.0f, 1.0f - (_p.y - r.getY()) / r.getHeight())};
	m_dev.setJoystick(m_joy.x, m_joy.y);
}

void SkinnedLarc2Component::mouseDown(const juce::MouseEvent& e)
{
	const auto p = toSkin(e.position);
	for (size_t i = 0; i < m_skin.faders.size(); ++i)
		if (m_skin.faders[i].track.expanded(4.0f, 0.0f).contains(p))
		{
			m_faderDrag = int(i);
			m_dev.setFaderTouched(m_skin.faders[i].index, true);  // LARC2 faders are touch-sensitive
			setFaderFrom(i, p);
			repaint();
			return;
		}
	if (m_skin.joystick.present && m_skin.joystick.r.expanded(m_skin.joystick.knobSize * 0.5f).contains(p))
	{
		m_joyDrag = true;
		setJoyFrom(p);
		repaint();
		return;
	}
	for (size_t i = 0; i < m_skin.keys.size(); ++i)
		if (m_skin.keys[i].r.contains(p))
		{
			m_keyDown = int(i);
			m_dev.pressKey(m_skin.keys[i].name.toStdString(), true);
			repaint();
			return;
		}
}

void SkinnedLarc2Component::mouseDrag(const juce::MouseEvent& e)
{
	const auto p = toSkin(e.position);
	if (m_faderDrag >= 0) { setFaderFrom(size_t(m_faderDrag), p); repaint(); }
	else if (m_joyDrag) { setJoyFrom(p); repaint(); }
}

void SkinnedLarc2Component::mouseUp(const juce::MouseEvent&)
{
	if (m_faderDrag >= 0) m_dev.setFaderTouched(m_skin.faders[size_t(m_faderDrag)].index, false);
	if (m_keyDown >= 0) m_dev.pressKey(m_skin.keys[size_t(m_keyDown)].name.toStdString(), false);
	m_faderDrag = m_keyDown = -1;
	m_joyDrag = false;  // the joystick stays where it was left
	repaint();
}
