#include "PluginProcessor.h"
#include "PluginEditor.h"

L960AudioProcessor::L960AudioProcessor()
	: AudioProcessor(BusesProperties()
		.withInput("Input", juce::AudioChannelSet::stereo(), true)
		.withOutput("Output", juce::AudioChannelSet::stereo(), true)),
	  params(*this, nullptr, "params", createLayout())
{
	m_mix = params.getRawParameterValue("mix");
	m_inGain = params.getRawParameterValue("in");
	m_outGain = params.getRawParameterValue("out");
	m_outPair = params.getRawParameterValue("outpair");
	m_engine.setMode(l960::Engine::Mode::HostDriven);

	// Developer convenience: preload firmware from L960_FIRMWARE_DIR
	if (const char* dir = std::getenv("L960_FIRMWARE_DIR"))
	{
		juce::String err;
		if (juce::File(dir).isDirectory()) loadFirmware(juce::File(dir), err);
	}
	startTimer(500);
}

L960AudioProcessor::~L960AudioProcessor()
{
	stopTimer();
	m_host.reset();  // stops the helper process and the bridge
	m_larc2.stop();
}

juce::AudioProcessorValueTreeState::ParameterLayout L960AudioProcessor::createLayout()
{
	using P = juce::AudioParameterFloat;
	juce::AudioProcessorValueTreeState::ParameterLayout l;
	l.add(std::make_unique<P>(juce::ParameterID{"mix", 1}, "Wet/Dry", 0.0f, 100.0f, 100.0f));
	l.add(std::make_unique<P>(juce::ParameterID{"in", 1}, "Input Level", juce::NormalisableRange<float>(-24.0f, 12.0f), 0.0f));
	l.add(std::make_unique<P>(juce::ParameterID{"out", 1}, "Output Level", juce::NormalisableRange<float>(-24.0f, 12.0f), 0.0f));
	// Which analog-out pair of the 960L feeds the plugin's stereo output. The host's default routing puts a
	// stereo fold-down of the machine on outputs 7/8 and the discrete channels on 1-5.
	l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"outpair", 1}, "Output Pair",
		juce::StringArray{"Outputs 7/8 (stereo fold)", "Outputs 1/2", "Outputs 3/4", "Outputs 5/6"}, 0));
	return l;
}

bool L960AudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
	return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() &&
	       (layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo() ||
	        layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono());
}

void L960AudioProcessor::prepareToPlay(const double sampleRate, const int samplesPerBlock)
{
	m_wet.setSize(8, samplesPerBlock);
	m_sampleRate = sampleRate;
}

void L960AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
	juce::ScopedNoDenormals noDenormals;
	const int n = buffer.getNumSamples();
	const int nIn = getTotalNumInputChannels(), nOut = getTotalNumOutputChannels();
	for (int c = nIn; c < nOut; ++c) buffer.clear(c, 0, n);
	if (m_wet.getNumSamples() < n) return;  // host broke the prepareToPlay contract

	const float inG = juce::Decibels::decibelsToGain(m_inGain->load());
	const float outG = juce::Decibels::decibelsToGain(m_outGain->load());
	const float wet = m_mix->load() / 100.0f;

	const juce::SpinLock::ScopedTryLockType lock(m_engineLock);
	if (!lock.isLocked()) return;  // firmware is being reloaded: pass dry signal through

	buffer.applyGain(inG);
	// plugin L/R -> 960L analog inputs 1/2
	const float* in[2] = {buffer.getReadPointer(0), buffer.getReadPointer(juce::jmin(1, nIn - 1))};
	float* out[8];
	for (int c = 0; c < 8; ++c) out[c] = m_wet.getWritePointer(c);
	m_engine.process(in, 2, out, 8, n);

	static constexpr int pairs[4] = {6, 0, 2, 4};
	const int first = pairs[juce::jlimit(0, 3, static_cast<int>(m_outPair->load()))];
	for (int c = 0; c < juce::jmin(nOut, 2); ++c)
	{
		auto* d = buffer.getWritePointer(c);
		const auto* w = m_wet.getReadPointer(first + c);
		for (int i = 0; i < n; ++i) d[i] = outG * (w[i] * wet + d[i] * (1.0f - wet));
	}
}

juce::File L960AudioProcessor::dataFolder()
{
	return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("L960Emu");
}

bool L960AudioProcessor::loadFirmware(const juce::File& folder, juce::String& error)
{
	std::string err;
	{
		const juce::SpinLock::ScopedLockType lock(m_engineLock);
		if (!m_engine.loadFirmware(folder.getFullPathName().toStdString(), &err)) { error = err; return false; }
	}
	m_firmwareFolder = folder;
	restartHost();
	return true;
}

void L960AudioProcessor::restartHost()
{
	m_host.reset();
	m_larc2.stop();
	m_hostError.clear();
	m_helperWanted = false;
	if (!m_firmwareFolder.isDirectory()) return;

	const auto root = m_firmwareFolder.getFullPathName().toStdString();
	std::string err;
	if (!m_larc2.load(root, &err)) { m_hostError = "LARC2: " + juce::String(err); return; }
	m_larc2.start();

	m_host = std::make_unique<l960::HostSystem>(m_engine.card(), m_engine.cardLock(), m_larc2);
	if (!m_host->start(root, &err)) { m_hostError = juce::String(err); m_host.reset(); return; }
	m_helperWanted = true;  // launched once the LARC2 has booted its application (timerCallback)
}

void L960AudioProcessor::timerCallback()
{
	if (!m_host || !m_helperWanted || !m_larc2.linked()) return;
	m_helperWanted = false;
	std::string err;
	if (!m_host->launchHelper(m_firmwareFolder.getFullPathName().toStdString(), dataFolder().getFullPathName().toStdString(), &err))
		m_hostError = juce::String(err);
}

juce::String L960AudioProcessor::statusText() const
{
	if (!m_engine.firmware()) return "No firmware loaded. Choose the \"960L\" folder from the V4.x software CD.";
	juce::String s;
	s << "960L software: " << m_firmwareFolder.getFullPathName() << "\n";
	if (m_host) s << juce::String(m_host->status()) << (m_host->helperRunning() ? ", 960L host software running" : "");
	if (m_hostError.isNotEmpty()) s << "\n" << m_hostError;
	return s;
}

void L960AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
	auto state = params.copyState();
	state.setProperty("firmware", m_firmwareFolder.getFullPathName(), nullptr);
	state.setProperty("skin", m_skinFolder, nullptr);
	if (const auto xml = state.createXml()) copyXmlToBinary(*xml, destData);
}

void L960AudioProcessor::setStateInformation(const void* data, const int sizeInBytes)
{
	const auto xml = getXmlFromBinary(data, sizeInBytes);
	if (!xml) return;
	const auto state = juce::ValueTree::fromXml(*xml);
	params.replaceState(state);
	m_skinFolder = state.getProperty("skin").toString();
	juce::String err;
	const juce::File fw(state.getProperty("firmware").toString());
	if (fw.isDirectory() && fw != m_firmwareFolder) loadFirmware(fw, err);
}

juce::AudioProcessorEditor* L960AudioProcessor::createEditor() { return new L960AudioProcessorEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new L960AudioProcessor(); }
