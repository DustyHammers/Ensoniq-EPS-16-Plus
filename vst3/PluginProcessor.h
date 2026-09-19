#ifndef EPS16_VST3_PLUGIN_PROCESSOR_H
#define EPS16_VST3_PLUGIN_PROCESSOR_H

#include "EmulatorBridge.h"
#include "ProbeMachineSink.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

class Eps16PlusProcessor final : public juce::AudioProcessor {
public:
    Eps16PlusProcessor();

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String &) override {}

    void getStateInformation(juce::MemoryBlock &) override;
    void setStateInformation(const void *, int) override;
    bool restoreMachineSnapshot(const void *data, std::size_t size);

    bool enqueuePanelTransition(std::uint8_t code, bool pressed) {
        return bridge.enqueuePanelTransition(code, pressed);
    }
    bool enqueueKeyboardTransition(std::uint8_t note, std::uint8_t velocity,
                                   bool pressed) {
        return bridge.enqueueKeyboardTransition(note, velocity, pressed);
    }
    bool enqueueAnalog(unsigned int channel, std::uint16_t value) {
        return bridge.enqueueAnalog(channel, value);
    }
    void setResourcePath(const juce::Identifier &key, const juce::String &path);
    juce::String getResourcePath(const juce::Identifier &key) const;
    void refreshResourcePaths();
    bool insertOsDisk();
    bool insertDisk(const juce::File &diskFile);
    bool createBlankDisk();
    bool saveDisk(const juce::File &diskFile);
    bool blankDiskMounted() const;
    static juce::File defaultResourceDirectory();
    static juce::Array<juce::File> resourceSearchDirectories();
    std::uint64_t cpuCycles() const { return bridge.cpuCycles(); }
    bool machineReady() const { return machineSink.isReady(); }
    juce::String machineStatus() const { return machineSink.status(); }
    juce::String machineDisplay() const { return machineSink.display(); }
    int machineCursorStart() const { return machineSink.cursorStart(); }
    int machineCursorEnd() const { return machineSink.cursorEnd(); }
    std::uint32_t machineCursorSegmentMask() const {
        return machineSink.cursorSegmentMask();
    }
    std::uint32_t machineDecimalMask() const {
        return machineSink.decimalMask();
    }
    std::uint16_t machineIndicatorOn(unsigned int bank) const {
        return machineSink.indicatorOn(bank);
    }
    std::uint16_t machineIndicatorFlash(unsigned int bank) const {
        return machineSink.indicatorFlash(bank);
    }
    std::size_t illegalInstructions() const {
        return machineSink.illegalInstructions();
    }

    static const juce::Identifier romPathKey;
    static const juce::Identifier upperRomPathKey;
    static const juce::Identifier lowerRomPathKey;
    static const juce::Identifier kpcPathKey;
    static const juce::Identifier osDiskPathKey;
    static const juce::Identifier mountedDiskPathKey;
    static const juce::Identifier blankDiskMountedKey;

private:
    static constexpr std::size_t maximumMidiEvents = 1024;
    static constexpr std::size_t maximumSysExEvents = 32;
    static constexpr std::size_t maximumInputSysExBytes = 128;
    eps16::vst3::ProbeMachineSink machineSink;
    eps16::vst3::EmulatorBridge bridge{machineSink};
    eps16::vst3::HostMidiClock hostMidiClock;
    std::array<eps16::vst3::MidiEvent, maximumMidiEvents> midiEvents{};
    std::array<std::array<std::uint8_t, maximumInputSysExBytes>,
               maximumSysExEvents> sysExInput{};
    std::array<eps16::vst3::SysExOutputEvent,
               maximumSysExEvents> sysExOutput{};
    juce::ValueTree state{"EPS16PlusPrototype"};
    juce::MemoryBlock pendingMachineState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Eps16PlusProcessor)
};

#endif
