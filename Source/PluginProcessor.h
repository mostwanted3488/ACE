#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "AceDsp.h"

// Lock-free-ish mono ring buffer the UI reads to draw the scope spectrum.
struct ScopeTap
{
    static constexpr int size = 8192;   // power of two
    std::array<float, size> data {};
    std::atomic<int> writePos { 0 };

    void push (const float* const* ch, int numCh, int n) noexcept
    {
        int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < numCh; ++c) s += ch[c][i];
            data[(size_t) w] = numCh > 0 ? s / (float) numCh : 0.0f;
            w = (w + 1) & (size - 1);
        }
        writePos.store (w, std::memory_order_release);
    }

    // copies the most recent `n` samples (n <= size)
    void read (float* dest, int n) const noexcept
    {
        const int w = writePos.load (std::memory_order_acquire);
        int r = (w - n) & (size - 1);
        for (int i = 0; i < n; ++i)
        {
            dest[i] = data[(size_t) r];
            r = (r + 1) & (size - 1);
        }
    }
};

class AceAudioProcessor final : public juce::AudioProcessor,
                                private juce::AudioProcessorValueTreeState::Listener
{
public:
    AceAudioProcessor();
    ~AceAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ACE"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    ace::Engine engine;
    ScopeTap tapIn, tapOut;

    // output peaks since the UI last read them
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };

    // UI-only state saved with the project
    juce::String getSkin() const;
    void setSkin (const juce::String& s);
    int  getAceCount() const;
    void setAceCount (int n);

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    void parameterChanged (const juce::String& id, float newValue) override;
    void pushParamsToEngine();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AceAudioProcessor)
};
