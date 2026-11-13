// Headless smoke test: loads the firmware, runs two algorithms through processBlock, saves state.
// Usage: plugin_smoke <960L folder>
#include "PluginProcessor.h"
#include "Larc2Skin.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI init;
    std::unique_ptr<juce::AudioProcessor> p(new L960AudioProcessor());
    auto* proc = dynamic_cast<L960AudioProcessor*>(p.get());
    p->setPlayConfigDetails(2, 2, 48000.0, 512);
    p->prepareToPlay(48000.0, 512);
    juce::String err;
    if (!proc->loadFirmware(juce::File(argv[1]), err)) { std::printf("load failed: %s\n", err.toRawUTF8()); return 1; }
    // Optional: let the 960L boot (helper started via L960_HELPER_CMD on non-Windows) before measuring
    if (const char* w = std::getenv("L960_SMOKE_WAIT"))
        for (int t = 0; t < std::atoi(w); ++t) {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1000);
            juce::AudioBuffer<float> b(2, 512); b.clear(); juce::MidiBuffer m; p->processBlock(b, m);
            if (t % 10 == 9) std::printf("t=%d %s\n", t + 1, proc->statusText().replace("\n", " | ").toRawUTF8());
        }
    for (auto alg : {"host-driven"}) {
        juce::AudioBuffer<float> b(2, 512); b.clear(); b.setSample(0, 0, 0.5f); b.setSample(1, 0, 0.5f);
        juce::MidiBuffer m; double e = 0;
        for (int k = 0; k < 100; ++k) { p->processBlock(b, m); for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) e += b.getSample(c, i) * b.getSample(c, i); b.clear(); }
        std::printf("%s: output energy over 100 blocks = %.4f\n%s\n", alg, e, proc->statusText().toRawUTF8());
    }
    // Optional: write a 4 s stereo impulse response (outputs 7/8) to a WAV file
    if (const char* wav = std::getenv("L960_SMOKE_WAV")) {
        juce::AudioBuffer<float> all(2, 48000 * 4); all.clear();
        juce::MidiBuffer m;
        for (int k = 0; k < all.getNumSamples(); k += 512) {
            juce::AudioBuffer<float> b(2, 512); b.clear();
            if (k == 0) { b.setSample(0, 0, 0.5f); b.setSample(1, 0, 0.5f); }
            p->processBlock(b, m);
            for (int c = 0; c < 2; ++c) all.copyFrom(c, k, b, c, 0, 512);
        }
        juce::File f(wav); f.deleteFile();
        juce::WavAudioFormat fmt;
        if (auto w = std::unique_ptr<juce::AudioFormatWriter>(fmt.createWriterFor(new juce::FileOutputStream(f), 48000.0, 2, 24, {}, 0)))
            w->writeFromAudioSampleBuffer(all, 0, all.getNumSamples());
        std::printf("wrote %s (peak %.3f)\n", wav, all.getMagnitude(0, all.getNumSamples()));
    }
    // Optional: render the skinned remote to a PNG (checks a skin's layout)
    if (const char* skinDir = std::getenv("L960_SMOKE_SKIN")) {
        Larc2Skin skin;
        const auto e = skin.load(juce::File(skinDir));
        if (e.isNotEmpty()) { std::printf("skin: %s\n", e.toRawUTF8()); return 1; }
        SkinnedLarc2Component c(proc->larc2(), std::move(skin));
        c.setSize(1400, 760);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1500);
        const auto snap = c.createComponentSnapshot(c.getLocalBounds());
        juce::File out(std::getenv("L960_SMOKE_SNAP") ? std::getenv("L960_SMOKE_SNAP") : "skin.png"); out.deleteFile();
        juce::FileOutputStream os(out); juce::PNGImageFormat().writeImageToStream(snap, os);
        std::printf("skin snapshot: %s\n", out.getFullPathName().toRawUTF8());
    }
    juce::MemoryBlock st; p->getStateInformation(st); std::printf("state %zu bytes\n", st.getSize());
    return 0;
}
