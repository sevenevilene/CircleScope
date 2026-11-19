#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ScopeSource.h"
#include "ScopeCurve.h"
#include "CurveAnalyser.h"
#include "InstanceRegistry.h"
#include "NetSender.h"

class CircleScopeProcessor : public juce::AudioProcessor,
                             private juce::Timer
{
public:
    CircleScopeProcessor();
    ~CircleScopeProcessor() override;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
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

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Stereo on this instance's own scopes, the editor's and the full
    // screen one: scope::Stereo, how deep in percent, and how often Wiggle
    // changes eye, a second. It is about the
    // screen, not the scene, so it is not a parameter and is not sent to the
    // network; a wall or a node has its own. Saved with the session.
    std::atomic<int> stereoMode { 0 }, stereoDepth { 100 }, stereoRate { scope::wiggleRate };

    // Steps a choice or toggle parameter to its next value (keyboard shortcuts).
    void cycleParameter (int paramIndex);

    // Nodes and walls on the network currently receiving this instance.
    int getNumNetworkTargets() const { return sender.getNumTargets(); }

    // The other instances in this host whose audio is added to this one's
    // drawing (see CurveAnalyser), by their ids. The choice is saved with
    // the session.
    juce::Uuid getInstanceId() const { return instanceId; }
    std::vector<juce::Uuid> getPartnerIds() const;
    void setPartner (const juce::Uuid&, bool shouldBeSummed);
    std::vector<std::shared_ptr<const ScopeSource>> getPartners() const;   // those of them that exist now

    void updateTrackProperties (const TrackProperties&) override;

    // Shared, because other instances may be adding this one to their drawings.
    const std::shared_ptr<ScopeSource> sourceHolder = std::make_shared<ScopeSource>();

    juce::AudioProcessorValueTreeState apvts;
    ScopeSource& source = *sourceHolder;   // this instance's audio; its parameters point into apvts
    scope::CurveFeed feed { source };      // its drawing: what the scopes show and the network gets

private:
    void timerCallback() override;
    void updateClock();

    bool bindSourceToParameters();
    NetSender::StreamSwitches streamSwitches();

    juce::Uuid instanceId;
    juce::CriticalSection partnerLock;
    std::vector<juce::Uuid> partnerIds;

    const bool sourceBound;   // initialised before the analyser and sender, whose threads read source
    TestTone testTone;

    // MIDI CCs arrive on the audio thread; the timer applies them to the
    // parameters on the message thread. -1 = nothing pending.
    std::atomic<int> pendingCc[scope::numParams];

    CurveAnalyser analyser { source, feed, [this] { return getPartners(); } };
    NetSender sender;   // declared last: its thread reads the feed

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeProcessor)
};
