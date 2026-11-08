#include "PluginProcessor.h"
#include "ProbeMachine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::vector<char> readFile(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto size = input.tellg();
    if (size <= 0) return {};
    std::vector<char> result(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input.read(result.data(), size)) return {};
    return result;
}

static void configure(Eps16PlusProcessor &processor, char **argv) {
    processor.setResourcePath(Eps16PlusProcessor::romPathKey, argv[2]);
    processor.setResourcePath(Eps16PlusProcessor::kpcPathKey, argv[3]);
    processor.setResourcePath(Eps16PlusProcessor::osDiskPathKey, argv[4]);
}

int main(int argc, char **argv) {
    if (argc != 7 || (std::string(argv[1]) != "write" &&
                      std::string(argv[1]) != "read")) {
        std::cerr << "usage: " << argv[0]
                  << " write|read ROM KPC OS RAW_SNAPSHOT VST_STATE\n";
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI gui;
    if (std::string(argv[1]) == "write") {
        Eps16PlusProcessor processor;
        configure(processor, argv);
        processor.prepareToPlay(48000.0, 512);
        const auto raw = readFile(argv[5]);
        if (raw.empty() ||
            !processor.restoreMachineSnapshot(raw.data(), raw.size())) {
            std::cerr << "raw restore failed\n";
            return 1;
        }
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        std::ofstream output(argv[6], std::ios::binary);
        if (!output.write(static_cast<const char *>(state.getData()),
                          static_cast<std::streamsize>(state.getSize())))
            return 1;
        std::cout << "vst_state_size=" << state.getSize() << '\n';
        return 0;
    }

    const auto saved = readFile(argv[6]);
    if (saved.empty()) return 1;
    Eps16PlusProcessor processor;
    processor.setStateInformation(saved.data(), static_cast<int>(saved.size()));
    processor.prepareToPlay(48000.0, 512);
    const auto display = processor.machineDisplay();
    if (!display.startsWith("MODE=FORWARD-NO LOOP")) return 1;
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    float restoredPeak = 0.0f;
    for (unsigned int block = 0; block < 100; ++block) {
        audio.clear();
        processor.processBlock(audio, midi);
        restoredPeak = std::max(
            restoredPeak, std::max(audio.getMagnitude(0, 0, 512),
                                   audio.getMagnitude(1, 0, 512)));
    }
    if (restoredPeak != 0.0f) {
        std::cerr << "restored preset emitted held-note audio: "
                  << restoredPeak << '\n';
        return 1;
    }
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
    float peak = 0.0f;
    for (unsigned int block = 0; block < 100; ++block) {
        audio.clear();
        processor.processBlock(audio, midi);
        midi.clear();
        peak = std::max(peak, std::max(audio.getMagnitude(0, 0, 512),
                                       audio.getMagnitude(1, 0, 512)));
    }
    std::cout << "status=" << processor.machineStatus()
              << " display=|" << display << "| restored_peak=" << restoredPeak
              << " played_peak=" << peak << '\n';
    return peak >= 0.00001f && !processor.illegalInstructions() ? 0 : 1;
}
