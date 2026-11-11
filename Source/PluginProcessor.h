#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "RingBuffer.h"

class CircleScopeProcessor : public juce::AudioProcessor
{
public:
    CircleScopeProcessor();

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
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

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // 2^17 samples: ~0.68 s at 192 kHz, more than the render thread ever reads.
    static constexpr size_t ringSize = 1u << 17;
    ScopeRingBuffer rings[2] { ScopeRingBuffer (ringSize), ScopeRingBuffer (ringSize) };
    std::atomic<double> currentSampleRate { 48000.0 };

    juce::AudioProcessorValueTreeState apvts;

private:
    // Built-in test signal: L = 20 Hz + 60 Hz, R = 20 Hz + 60.5 Hz.
    double phase20 = 0.0, phase60 = 0.0, phase605 = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircleScopeProcessor)
};
