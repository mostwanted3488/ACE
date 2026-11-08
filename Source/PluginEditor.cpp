#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    std::vector<std::byte> toBytes (const char* data, int size)
    {
        std::vector<std::byte> v ((size_t) size);
        std::memcpy (v.data(), data, (size_t) size);
        return v;
    }

    juce::RangedAudioParameter* paramFor (AceAudioProcessor& p, const juce::String& id)
    {
        return p.apvts.getParameter (id);
    }
}

static juce::WebBrowserComponent::Options baseOptions()
{
    auto o = juce::WebBrowserComponent::Options{};
   #if JUCE_WINDOWS
    // WebView2 needs a writable profile folder (plugins live in Program Files)
    o = o.withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
         .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                      .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                               .getChildFile ("ACE_WebView2")));
   #endif
    return o;
}

AceAudioProcessorEditor::AceAudioProcessorEditor (AceAudioProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      web (baseOptions()
               .withNativeIntegrationEnabled()
               .withKeepPageLoadedWhenBrowserIsHidden()
               .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
               .withEventListener ("ready", [this] (const juce::var&)
               {
                   pageReady = true;
                   web.emitEventIfBrowserIsVisible ("init", buildInit());
               })
               .withEventListener ("set",     [this] (const juce::var& v) { onSet (v); })
               .withEventListener ("gesture", [this] (const juce::var& v) { onGesture (v); })
               .withEventListener ("shoot",   [this] (const juce::var& v)
               {
                   const int slot = (int) v.getProperty ("slot", -1);
                   if (juce::isPositiveAndBelow (slot, ace::numDancers))
                       proc.engine.shootCount[(size_t) slot].fetch_add (1);
               })
               .withEventListener ("band",    [this] (const juce::var& v)
               {
                   const int slot = (int) v.getProperty ("slot", -1);
                   const float f  = (float) (double) v.getProperty ("freq", 0.0);
                   if (juce::isPositiveAndBelow (slot, ace::numDancers) && f > 10.0f)
                       proc.engine.bandFreq[(size_t) slot].store (f);
               })
               .withEventListener ("ui",      [this] (const juce::var& v)
               {
                   if (v.hasProperty ("skin")) proc.setSkin (v.getProperty ("skin", "stock").toString());
                   if (v.hasProperty ("ace"))  proc.setAceCount ((int) v.getProperty ("ace", 0));
               }))
{
    addAndMakeVisible (web);
    web.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setResizable (false, false);
    setSize (960, 580);
    startTimerHz (30);
}

AceAudioProcessorEditor::~AceAudioProcessorEditor()
{
    stopTimer();
}

void AceAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff070708));
}

void AceAudioProcessorEditor::resized()
{
    web.setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> AceAudioProcessorEditor::getResource (const juce::String& url)
{
    if (url == "/" || url == "/index.html")
        return juce::WebBrowserComponent::Resource { toBytes (BinaryData::index_html, BinaryData::index_htmlSize),
                                                     "text/html" };
    return std::nullopt;
}

juce::var AceAudioProcessorEditor::buildInit() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("skin", proc.getSkin());
    o->setProperty ("ace", proc.getAceCount());
    o->setProperty ("charge", proc.apvts.getRawParameterValue ("charge")->load());
    o->setProperty ("mode", (int) proc.apvts.getRawParameterValue ("mode")->load());
    o->setProperty ("mix", proc.apvts.getRawParameterValue ("mix")->load());
    o->setProperty ("safety", proc.apvts.getRawParameterValue ("safety")->load() > 0.5f);
    return juce::var (o);
}

void AceAudioProcessorEditor::onSet (const juce::var& v)
{
    const auto id = v.getProperty ("id", "").toString();
    auto* param = paramFor (proc, id);
    if (param == nullptr) return;

    const float value = (float) (double) v.getProperty ("value", 0.0);
    const float norm = param->convertTo0to1 (param->getNormalisableRange().snapToLegalValue (value));
    if (std::abs (param->getValue() - norm) > 1.0e-6f)
        param->setValueNotifyingHost (norm);
}

void AceAudioProcessorEditor::onGesture (const juce::var& v)
{
    auto* param = paramFor (proc, v.getProperty ("id", "").toString());
    if (param == nullptr) return;
    if ((bool) v.getProperty ("start", false)) param->beginChangeGesture();
    else                                        param->endChangeGesture();
}

void AceAudioProcessorEditor::computeSpectrum (ScopeTap& tap, std::array<float, 96>& smooth, juce::Array<juce::var>& out)
{
    std::fill (fftData.begin(), fftData.end(), 0.0f);
    tap.read (fftData.data(), fftSize);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
    const double nyq = sr * 0.5;
    const int half = fftSize / 2;

    out.ensureStorageAllocated (96);
    for (int i = 0; i < 96; ++i)
    {
        const double f0 = 30.0 * std::pow (20000.0 / 30.0, (double) i / 96.0);
        const double f1 = 30.0 * std::pow (20000.0 / 30.0, (double) (i + 1) / 96.0);
        int a = (int) std::floor (f0 / nyq * half);
        int b = std::max (a + 1, (int) std::ceil (f1 / nyq * half));
        a = juce::jlimit (0, half - 1, a);
        b = juce::jlimit (a + 1, half, b);

        float m = 0.0f;
        for (int j = a; j < b; ++j) m = std::max (m, fftData[(size_t) j] / (float) fftSize);

        // same time smoothing as the browser analyser (0.72)
        smooth[(size_t) i] = 0.72f * smooth[(size_t) i] + 0.28f * m;
        const float db = 20.0f * std::log10 (std::max (smooth[(size_t) i], 1.0e-9f));
        out.add (std::round (std::max (-72.0f, db + 18.0f) * 10.0f) / 10.0f);
    }
}

void AceAudioProcessorEditor::timerCallback()
{
    if (! pageReady) return;

    auto* o = new juce::DynamicObject();
    o->setProperty ("charge", proc.apvts.getRawParameterValue ("charge")->load());
    o->setProperty ("mode", (int) proc.apvts.getRawParameterValue ("mode")->load());
    o->setProperty ("mix", proc.apvts.getRawParameterValue ("mix")->load());
    o->setProperty ("safety", proc.apvts.getRawParameterValue ("safety")->load() > 0.5f);

    juce::Array<juce::var> peak;
    peak.add (proc.peakL.exchange (0.0f));
    peak.add (proc.peakR.exchange (0.0f));
    o->setProperty ("peak", peak);

    juce::Array<juce::var> specIn, specOut;
    computeSpectrum (proc.tapIn, smoothIn, specIn);
    computeSpectrum (proc.tapOut, smoothOut, specOut);
    o->setProperty ("specIn", specIn);
    o->setProperty ("specOut", specOut);

    web.emitEventIfBrowserIsVisible ("state", juce::var (o));
}
