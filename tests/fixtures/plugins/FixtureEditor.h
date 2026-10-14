#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Fixture plugins have no custom editor; hosts use a generic one.
#define WEFT_FIXTURE_NO_EDITOR                                                                 \
    using juce::AudioProcessor::processBlock;                                                  \
    bool hasEditor() const override { return false; }                                          \
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }                    \
    const juce::String getName() const override { return JucePlugin_Name; }                    \
    double getTailLengthSeconds() const override { return 0.0; }                               \
    int getNumPrograms() override { return 1; }                                                \
    int getCurrentProgram() override { return 0; }                                             \
    void setCurrentProgram (int) override {}                                                   \
    const juce::String getProgramName (int) override { return {}; }                            \
    void changeProgramName (int, const juce::String&) override {}
