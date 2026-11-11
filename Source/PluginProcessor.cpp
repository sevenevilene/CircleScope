#include "PluginProcessor.h"
#include "PluginEditor.h"

CircleScopeProcessor::CircleScopeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout CircleScopeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                        StringArray { "Quadrature", "Delay embed" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "window", 1 }, "Window",
                                                       NormalisableRange<float> (5.0f, 300.0f, 0.1f, 0.5f), 60.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "delay", 1 }, "Delay",
                                                       NormalisableRange<float> (0.05f, 50.0f, 0.01f, 0.4f), 12.5f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "colour", 1 }, "Colour",
                                                        StringArray { "Frequency hue", "Fixed" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "test", 1 }, "Test tone",
                                                      JUCEApplicationBase::isStandaloneApp()));
    return layout;
}

void CircleScopeProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate.store (sampleRate);
}

bool CircleScopeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void CircleScopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();

    // Audio passes through untouched; we only copy it for display.
    if (apvts.getRawParameterValue ("test")->load() > 0.5f)
    {
        const double sr = currentSampleRate.load();
        const double inc20 = juce::MathConstants<double>::twoPi * 20.0 / sr;
        const double inc60 = juce::MathConstants<double>::twoPi * 60.0 / sr;
        const double inc605 = juce::MathConstants<double>::twoPi * 60.5 / sr;

        constexpr int chunk = 256;
        float left[chunk], right[chunk];
        for (int pos = 0; pos < numSamples; pos += chunk)
        {
            const int n = juce::jmin (chunk, numSamples - pos);
            for (int i = 0; i < n; ++i)
            {
                const float base = 0.7f * (float) std::sin (phase20);
                left[i] = base + 0.3f * (float) std::sin (phase60);
                right[i] = base + 0.3f * (float) std::sin (phase605);
                phase20 = std::fmod (phase20 + inc20, juce::MathConstants<double>::twoPi);
                phase60 = std::fmod (phase60 + inc60, juce::MathConstants<double>::twoPi);
                phase605 = std::fmod (phase605 + inc605, juce::MathConstants<double>::twoPi);
            }
            rings[0].push (left, n);
            rings[1].push (right, n);
        }
        return;
    }

    if (numIn == 0)
        return;

    rings[0].push (buffer.getReadPointer (0), numSamples);
    rings[1].push (buffer.getReadPointer (numIn > 1 ? 1 : 0), numSamples);
}

juce::AudioProcessorEditor* CircleScopeProcessor::createEditor()
{
    return new CircleScopeEditor (*this);
}

void CircleScopeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void CircleScopeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CircleScopeProcessor();
}
