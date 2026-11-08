#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ids
{
    static const juce::String charge { "charge" };
    static const juce::String mode   { "mode" };
    static const juce::String mix    { "mix" };
    static const juce::String safety { "safety" };
}

juce::AudioProcessorValueTreeState::ParameterLayout AceAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ids::charge, 1 }, "Charge",
                                                       NormalisableRange<float> (0.0f, 100.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ids::mode, 1 }, "Ammo",
                                                        StringArray { "SOFT", "HARD", "BIT" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ids::mix, 1 }, "Suppressor (Mix)",
                                                       NormalisableRange<float> (0.0f, 100.0f, 0.01f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ids::safety, 1 }, "Safety (Bypass)", false));
    return layout;
}

AceAudioProcessor::AceAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ACE", createLayout())
{
    for (auto* id : { &ids::charge, &ids::mode, &ids::mix, &ids::safety })
        apvts.addParameterListener (*id, this);

    apvts.state.setProperty ("skin", "stock", nullptr);
    apvts.state.setProperty ("ace", 0, nullptr);
    pushParamsToEngine();
}

AceAudioProcessor::~AceAudioProcessor()
{
    for (auto* id : { &ids::charge, &ids::mode, &ids::mix, &ids::safety })
        apvts.removeParameterListener (*id, this);
}

void AceAudioProcessor::parameterChanged (const juce::String&, float)
{
    pushParamsToEngine();
}

void AceAudioProcessor::pushParamsToEngine()
{
    engine.targetCharge.store (apvts.getRawParameterValue (ids::charge)->load() / 100.0f);
    engine.targetMode.store ((int) apvts.getRawParameterValue (ids::mode)->load());
    engine.targetMix.store (apvts.getRawParameterValue (ids::mix)->load() / 100.0f);
    engine.safety.store (apvts.getRawParameterValue (ids::safety)->load() > 0.5f);
}

bool AceAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void AceAudioProcessor::prepareToPlay (double sampleRate, int)
{
    pushParamsToEngine();
    engine.prepare (sampleRate, getTotalNumOutputChannels());
    engine.reset();
}

void AceAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();

    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);

    tapIn.push (buffer.getArrayOfReadPointers(), numOut, n);

    pushParamsToEngine();
    engine.process (buffer.getArrayOfWritePointers(), numOut, n);

    tapOut.push (buffer.getArrayOfReadPointers(), numOut, n);

    const float pl = buffer.getMagnitude (0, 0, n);
    const float pr = numOut > 1 ? buffer.getMagnitude (1, 0, n) : pl;
    if (pl > peakL.load()) peakL.store (pl);
    if (pr > peakR.load()) peakR.store (pr);
}

juce::String AceAudioProcessor::getSkin() const       { return apvts.state.getProperty ("skin", "stock").toString(); }
void AceAudioProcessor::setSkin (const juce::String& s) { apvts.state.setProperty ("skin", s, nullptr); }
int  AceAudioProcessor::getAceCount() const           { return (int) apvts.state.getProperty ("ace", 0); }
void AceAudioProcessor::setAceCount (int n)           { apvts.state.setProperty ("ace", n, nullptr); }

void AceAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AceAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    pushParamsToEngine();
}

juce::AudioProcessorEditor* AceAudioProcessor::createEditor()
{
    return new AceAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AceAudioProcessor();
}
