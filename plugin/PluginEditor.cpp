#include "PluginEditor.h"

L960AudioProcessorEditor::L960AudioProcessorEditor(L960AudioProcessor& p) : AudioProcessorEditor(&p), m_proc(p)
{
	addAndMakeVisible(m_skinButton);
	m_skinButton.onClick = [this] { chooseSkin(); };
	addAndMakeVisible(m_locate);
	m_locate.onClick = [this] { chooseFirmware(); };

	addAndMakeVisible(m_outPair);
	if (auto* c = dynamic_cast<juce::AudioParameterChoice*>(p.params.getParameter("outpair"))) m_outPair.addItemList(c->choices, 1);
	m_outPairA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.params, "outpair", m_outPair);
	addAndMakeVisible(m_restart);
	m_restart.onClick = [this] { m_proc.restartHost(); refresh(); };

	addAndMakeVisible(m_status);
	m_status.setJustificationType(juce::Justification::topLeft);
	m_status.setFont(juce::FontOptions(13.0f));

	for (auto* s : {&m_mix, &m_in, &m_out})
	{
		s->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
		s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 72, 18);
		addAndMakeVisible(*s);
	}
	for (auto* l : {&m_mixLabel, &m_inLabel, &m_outLabel})
	{
		l->setJustificationType(juce::Justification::centred);
		addAndMakeVisible(*l);
	}
	m_mixA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.params, "mix", m_mix);
	m_inA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.params, "in", m_in);
	m_outA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.params, "out", m_out);

	applySkin();
	setSize(1040, 720);
	startTimerHz(2);
	setResizable(true, true);
	setResizeLimits(900, 640, 1600, 1100);
	refresh();
}

void L960AudioProcessorEditor::chooseFirmware()
{
	m_chooser = std::make_unique<juce::FileChooser>("Select the 960L folder from the software CD", m_proc.firmwareFolder());
	m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
		[this](const juce::FileChooser& fc) {
			const auto dir = fc.getResult();
			if (!dir.isDirectory()) return;
			juce::String err;
			if (!m_proc.loadFirmware(dir, err)) m_status.setText("Could not load firmware: " + err, juce::dontSendNotification);
			else refresh();
		});
}

void L960AudioProcessorEditor::chooseSkin()
{
	juce::PopupMenu m;
	m.addItem(1, "Load skin folder...");
	m.addItem(2, "Built-in look", true, m_proc.skinFolder().isEmpty());
	m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(m_skinButton), [this](int r) {
		if (r == 2) { m_proc.setSkinFolder({}); applySkin(); }
		if (r != 1) return;
		m_chooser = std::make_unique<juce::FileChooser>("Select a LARC2 skin folder (with skin.json)", juce::File(m_proc.skinFolder()));
		m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
			[this](const juce::FileChooser& fc) {
				const auto dir = fc.getResult();
				if (!dir.isDirectory()) return;
				m_proc.setSkinFolder(dir.getFullPathName());
				applySkin();
			});
	});
}

void L960AudioProcessorEditor::applySkin()
{
	if (m_remote) removeChildComponent(m_remote.get());
	m_remote.reset();
	if (m_proc.skinFolder().isNotEmpty())
	{
		Larc2Skin skin;
		const auto err = skin.load(juce::File(m_proc.skinFolder()));
		if (err.isEmpty()) m_remote = std::make_unique<SkinnedLarc2Component>(m_proc.larc2(), std::move(skin));
		else m_status.setText("Skin not loaded: " + err, juce::dontSendNotification);
	}
	if (!m_remote) m_remote = std::make_unique<Larc2Component>(m_proc.larc2());
	addAndMakeVisible(*m_remote);
	resized();
}

void L960AudioProcessorEditor::refresh()
{
	m_status.setText(m_proc.statusText(), juce::dontSendNotification);
}

void L960AudioProcessorEditor::timerCallback() { m_status.setText(m_proc.statusText(), juce::dontSendNotification); }

void L960AudioProcessorEditor::paint(juce::Graphics& g)
{
	g.fillAll(juce::Colour(0xff1d1f22));
	g.setColour(juce::Colours::white);
	g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
	g.drawText("960L Emulator", 16, 10, 300, 28, juce::Justification::centredLeft);
}

void L960AudioProcessorEditor::resized()
{
	auto r = getLocalBounds().reduced(16);
	r.removeFromTop(36);
	auto row = r.removeFromTop(28);
	m_locate.setBounds(row.removeFromLeft(180));
	row.removeFromLeft(12);
	m_outPair.setBounds(row.removeFromLeft(200));
	row.removeFromLeft(8);
	m_restart.setBounds(row.removeFromLeft(110));
	row.removeFromLeft(8);
	m_skinButton.setBounds(row.removeFromLeft(70));
	row.removeFromLeft(12);
	for (auto [s, l] : {std::pair{&m_in, &m_inLabel}, std::pair{&m_mix, &m_mixLabel}, std::pair{&m_out, &m_outLabel}})
	{
		l->setBounds(row.removeFromLeft(56));
		s->setSliderStyle(juce::Slider::LinearHorizontal);
		s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
		s->setNumDecimalPlacesToDisplay(1);
		s->setBounds(row.removeFromLeft(130));
	}
	r.removeFromTop(6);
	m_status.setBounds(r.removeFromTop(48));
	r.removeFromTop(6);
	if (m_remote) m_remote->setBounds(r);
}
