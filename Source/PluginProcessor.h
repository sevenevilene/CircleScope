#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ScopeSource.h"
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

    // Steps a choice or toggle parameter to its next value (keyboard shortcuts).
    void cycleParameter (int paramIndex);

    // Nodes on the network currently receiving this instance's stream.
    int getNumNetworkTargets() const { return sender.getNumTargets(); }

    juce::AudioProcessorValueTreeState apvts;
    ScopeSource source;   // what the scopes draw; its parameters point into apvts

private:
    void timerCallback() override;

    bool bindSourceToParameters();

    const bool sourceBound;   // initialised before sender, whose thread reads source
    TestTone testTone;

    // MIDI CCs arrive on the audio thread; the timer applies them to the
    // parameters on the message thread. -1 = nothing pending.
    std::atomic<int> pendingCc[scope::numParams];

    NetSender sender;   // declared last: its thread reads source

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeProcessor)
};
