#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // "netstream1".."netstream16": the streams this instance is on.
    juce::String streamParamId (int stream) { return "netstream" + juce::String (stream); }
}

CircleScopeProcessor::CircleScopeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      sourceBound (bindSourceToParameters()),
      sender (source, streamSwitches())
{
    juce::ignoreUnused (sourceBound);
    startTimerHz (60);
}

bool CircleScopeProcessor::bindSourceToParameters()
{
    for (int i = 0; i < scope::numParams; ++i)
    {
        source.param[i] = apvts.getRawParameterValue (scope::paramTable[i].id);
        pendingCc[i].store (-1);
    }
    return true;
}

NetSender::StreamSwitches CircleScopeProcessor::streamSwitches()
{
    NetSender::StreamSwitches switches;
    for (int i = 0; i < net::maxStreams; ++i)
        switches[(size_t) i] = apvts.getRawParameterValue (streamParamId (i + 1));
    return switches;
}

CircleScopeProcessor::~CircleScopeProcessor()
{
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout CircleScopeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < scope::numParams; ++i)
    {
        const auto& info = scope::paramTable[i];
        const ParameterID id { info.id, 1 };

        switch (info.kind)
        {
            case scope::ParamKind::choice:
                layout.add (std::make_unique<AudioParameterChoice> (id, info.name,
                                                                    StringArray::fromTokens (info.choices, "|", ""),
                                                                    (int) info.def));
                break;
            case scope::ParamKind::real:
                layout.add (std::make_unique<AudioParameterFloat> (id, info.name,
                                                                   NormalisableRange<float> (info.min, info.max, info.step, info.skew),
                                                                   info.def,
                                                                   AudioParameterFloatAttributes().withLabel (info.label)));
                break;
            case scope::ParamKind::toggle:
                // The standalone app starts on the test tone so it shows something.
                layout.add (std::make_unique<AudioParameterBool> (id, info.name,
                                                                  i == scope::pTest ? JUCEApplicationBase::isStandaloneApp()
                                                                                    : info.def > 0.5f));
                break;
        }
    }

    // Which network streams this instance is on. On a wall a stream is a
    // cell, so automating these moves the instance around the grid.
    for (int i = 1; i <= net::maxStreams; ++i)
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { streamParamId (i), 1 }, "Stream " + String (i), false));
    return layout;
}

void CircleScopeProcessor::cycleParameter (int paramIndex)
{
    if (auto* param = apvts.getParameter (scope::paramTable[paramIndex].id))
    {
        const int steps = juce::jmax (2, param->getNumSteps());
        const int current = juce::roundToInt (param->getValue() * (float) (steps - 1));
        param->beginChangeGesture();
        param->setValueNotifyingHost ((float) ((current + 1) % steps) / (float) (steps - 1));
        param->endChangeGesture();
    }
}

void CircleScopeProcessor::timerCallback()
{
    for (int i = 0; i < scope::numParams; ++i)
    {
        const int value = pendingCc[i].exchange (-1);
        if (value >= 0)
            if (auto* param = apvts.getParameter (scope::paramTable[i].id))
                param->setValueNotifyingHost ((float) value / 127.0f);
    }
}

void CircleScopeProcessor::prepareToPlay (double sampleRate, int)
{
    source.sampleRate.store (sampleRate);
}

bool CircleScopeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void CircleScopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Each parameter answers to one MIDI controller (see ScopeParams.h).
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (! message.isController())
            continue;
        for (int i = 0; i < scope::numParams; ++i)
            if (scope::paramTable[i].cc == message.getControllerNumber())
                pendingCc[i].store (message.getControllerValue());
    }

    const int numIn = getTotalNumInputChannels();
    for (int ch = numIn; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();

    // Audio passes through untouched; we only copy it for display.
    if (source.get (scope::pTest) > 0.5f)
    {
        constexpr int chunk = 256;
        float left[chunk], right[chunk];
        for (int pos = 0; pos < numSamples; pos += chunk)
        {
            const int n = juce::jmin (chunk, numSamples - pos);
            testTone.render (left, right, n, source.sampleRate.load());
            source.rings[0].push (left, n);
            source.rings[1].push (right, n);
        }
        return;
    }

    if (numIn == 0)
        return;

    source.rings[0].push (buffer.getReadPointer (0), numSamples);
    source.rings[1].push (buffer.getReadPointer (numIn > 1 ? 1 : 0), numSamples);
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
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            // A session saved when an instance could only be on one stream.
            if (const auto old = apvts.state.getChildWithProperty ("id", "netstream"); old.isValid())
            {
                if (auto* param = apvts.getParameter (streamParamId ((int) old["value"])))
                    param->setValueNotifyingHost (1.0f);
                apvts.state.removeChild (old, nullptr);
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CircleScopeProcessor();
}
