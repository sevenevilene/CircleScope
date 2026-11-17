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
      sender (feed, streamSwitches())
{
    juce::ignoreUnused (sourceBound);
    InstanceRegistry::get().add (instanceId, sourceHolder);
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

    // Another instance may go on reading this one's source for a moment:
    // let it read values of its own, not our parameters, which go with us.
    InstanceRegistry::get().remove (&source);
    source.detach();
}

std::vector<juce::Uuid> CircleScopeProcessor::getPartnerIds() const
{
    const juce::ScopedLock lock (partnerLock);
    return partnerIds;
}

void CircleScopeProcessor::setPartner (const juce::Uuid& id, bool shouldBeSummed)
{
    const juce::ScopedLock lock (partnerLock);
    partnerIds.erase (std::remove (partnerIds.begin(), partnerIds.end(), id), partnerIds.end());
    if (shouldBeSummed && id != instanceId)
        partnerIds.push_back (id);
}

std::vector<std::shared_ptr<const ScopeSource>> CircleScopeProcessor::getPartners() const
{
    std::vector<std::shared_ptr<const ScopeSource>> found;
    for (const auto& id : getPartnerIds())
        if (auto partner = InstanceRegistry::get().find (id))
            found.push_back (std::move (partner));
    return found;
}

void CircleScopeProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (properties.name.has_value())
        InstanceRegistry::get().setName (&source, *properties.name);
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

// Says where the block about to be pushed is on a clock the other instances
// in this host share: the song position while the host plays, its audio
// clock otherwise. That is what lets a receiver add instances up sample for
// sample, however their packets happen to arrive.
void CircleScopeProcessor::updateClock()
{
    int domain = (int) scope::Clock::none;
    juce::int64 position = 0;

    if (auto* playHead = getPlayHead())
    {
        if (const auto info = playHead->getPosition())
        {
            if (const auto samples = info->getTimeInSamples(); samples.hasValue() && info->getIsPlaying())
            {
                domain = (int) scope::Clock::timeline;
                position = *samples;
            }
            else if (const auto ns = info->getHostTimeNs())
            {
                domain = (int) scope::Clock::hostTime;
                position = (juce::int64) std::llround ((double) *ns * 1.0e-9 * source.sampleRate.load());
            }
        }
    }

    juce::int64 offset = position - (juce::int64) source.rings[1].getWritePosition();

    // The audio clock is in nanoseconds, so it lands a sample either side
    // from block to block: stay put unless it really moved.
    if (domain == (int) scope::Clock::hostTime && domain == source.clockDomain.load()
        && std::abs (offset - source.clockOffset.load()) <= 2)
        offset = source.clockOffset.load();

    source.clockOffset.store (offset);
    source.clockDomain.store (domain);
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
    const bool test = source.get (scope::pTest) > 0.5f;
    source.mono.store (numIn == 1 && ! test);
    updateClock();

    // Audio passes through untouched; we only copy it for display.
    if (test)
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
    auto state = apvts.copyState();
    state.setProperty ("instanceId", instanceId.toString(), nullptr);

    juce::StringArray partners;
    for (const auto& id : getPartnerIds())
        partners.add (id.toString());
    state.setProperty ("sumWith", partners.joinIntoString (","), nullptr);

    // The Field EQ's curve is drawn, not a parameter.
    juce::StringArray curve;
    for (auto& value : source.fieldCurve)
        curve.add (juce::String (value.load(), 4));
    state.setProperty ("fieldCurve", curve.joinIntoString (","), nullptr);

    // The scene's colour scale: every instance saves a copy, and the one
    // changed last wins when they are loaded (ColourScale.h).
    state.setProperty ("colourScale", scope::SceneColours::get().read().toString(), nullptr);
    state.setProperty ("colourScaleChanged", juce::String (scope::SceneColours::get().getStamp()), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void CircleScopeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            // Who this instance is, and whom it is summed with. A duplicated
            // track brings its original's id: the copy then keeps the new
            // one it was made with.
            const auto curve = juce::StringArray::fromTokens (apvts.state["fieldCurve"].toString(), ",", "");
            if (curve.size() == scope::fieldCurvePoints)
                for (int k = 0; k < scope::fieldCurvePoints; ++k)
                    source.fieldCurve[k].store (juce::jlimit (0.0f, 1.0f, curve[k].getFloatValue()));

            if (scope::ColourScale scale; scale.fromString (apvts.state["colourScale"].toString()))
                scope::SceneColours::get().offer (scale, apvts.state["colourScaleChanged"].toString().getLargeIntValue());

            if (const auto saved = apvts.state["instanceId"].toString(); saved.isNotEmpty())
                if (InstanceRegistry::get().changeId (&source, juce::Uuid (saved)))
                    instanceId = juce::Uuid (saved);

            {
                const juce::ScopedLock lock (partnerLock);
                partnerIds.clear();
                for (const auto& id : juce::StringArray::fromTokens (apvts.state["sumWith"].toString(), ",", ""))
                    if (id.isNotEmpty() && juce::Uuid (id) != instanceId)
                        partnerIds.push_back (juce::Uuid (id));
            }

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
