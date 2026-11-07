#include "PluginProcessor.h"
#include "PanelEditor.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#if JUCE_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
constexpr std::uint32_t vstStateMagic = 0x45505356U; // EPSV
constexpr std::uint32_t vstStateVersion = 1;

/* The OS boot sequence sets its own MIDI IN MODE default (MULTI) well
   before the machine reaches an idle, interactive state; every probe test
   in this repository treats 220,000,000 emulated CPU cycles after a fresh
   reset as "booted" for exactly this reason. Applying the POLY default
   before that point would just be overwritten by the OS moments later. */
constexpr std::uint64_t bootCompleteCpuCycles = 220000000;

void modulePathAnchor() {}

juce::File moduleFile() {
#if JUCE_WINDOWS
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&modulePathAnchor), &module))
        return {};
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    return juce::File(juce::String(path.data(), length));
#else
    Dl_info information{};
    if (dladdr(reinterpret_cast<const void *>(&modulePathAnchor), &information) &&
        information.dli_fname)
        return juce::File(juce::String::fromUTF8(information.dli_fname));
    return {};
#endif
}

juce::File firstExisting(const juce::File &directory,
                         std::initializer_list<const char *> names) {
    for (const auto *name : names) {
        const auto candidate = directory.getChildFile(name);
        if (candidate.existsAsFile()) return candidate;
    }
    return {};
}

juce::File firstFileWithSize(const juce::File &directory,
                             const juce::String &pattern,
                             std::int64_t expectedSize) {
    auto files = directory.findChildFiles(juce::File::findFiles, false, pattern);
    files.sort();
    for (const auto &file : files)
        if (file.getSize() == expectedSize) return file;
    return {};
}

struct SplitRomFiles {
    juce::File upper;
    juce::File lower;
};

constexpr auto upperRomSha256 =
    "91ac82ef131008bd714c3d831d22a374fce33695e899bcee1ded1410379394d0";
constexpr auto lowerRomSha256 =
    "1906e9929fe310bc08eb430ed5e83097542cabc8fd6cb57bd4ac7659847d1674";

bool isKnownSplitRom(const juce::File &upper, const juce::File &lower) {
    return upper.existsAsFile() && lower.existsAsFile() &&
           upper.getSize() == 65536 && lower.getSize() == 65536 &&
           juce::SHA256(upper).toHexString() == upperRomSha256 &&
           juce::SHA256(lower).toHexString() == lowerRomSha256;
}

SplitRomFiles findKnownSplitRom(const juce::File &directory) {
    SplitRomFiles result;
    auto files = directory.findChildFiles(juce::File::findFiles, false, "*");
    files.sort();
    for (const auto &file : files) {
        if (file.getSize() != 65536) continue;
        const auto digest = juce::SHA256(file).toHexString();
        if (digest == upperRomSha256) result.upper = file;
        if (digest == lowerRomSha256) result.lower = file;
        if (result.upper.existsAsFile() && result.lower.existsAsFile()) break;
    }
    return result;
}
} // namespace

const juce::Identifier Eps16PlusProcessor::romPathKey{"combinedRomPath"};
const juce::Identifier Eps16PlusProcessor::upperRomPathKey{"upperRomPath"};
const juce::Identifier Eps16PlusProcessor::lowerRomPathKey{"lowerRomPath"};
const juce::Identifier Eps16PlusProcessor::kpcPathKey{"kpcRomPath"};
const juce::Identifier Eps16PlusProcessor::osDiskPathKey{"osDiskPath"};
const juce::Identifier Eps16PlusProcessor::mountedDiskPathKey{"mountedDiskPath"};
const juce::Identifier Eps16PlusProcessor::blankDiskMountedKey{"blankDiskMounted"};

Eps16PlusProcessor::Eps16PlusProcessor()
    : AudioProcessor(BusesProperties()
          /* Keep the instrument's main input disabled and expose sampling as
             an auxiliary input. Hosts such as Ableton Live then present it as
             a routable sidechain source on a MIDI/instrument track. */
          .withInput("Main Input", juce::AudioChannelSet::stereo(), false)
          .withInput("Sampling Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Main Output", juce::AudioChannelSet::stereo(), true)
          .withOutput("OEX Bus 2", juce::AudioChannelSet::stereo(), false)
          .withOutput("OEX Bus 3", juce::AudioChannelSet::stereo(), false)
          .withOutput("OEX Aux 1", juce::AudioChannelSet::stereo(), false)) {
    refreshResourcePaths();
}

void Eps16PlusProcessor::prepareToPlay(double sampleRate,
                                       int maximumExpectedSamplesPerBlock) {
    refreshResourcePaths();
    // bus2Left, bus2Right, bus3Left, bus3Right, aux1Left, aux1Right.
    oexScratchBuffer.setSize(6, maximumExpectedSamplesPerBlock,
                             false, false, false);
    machineSink.configure(getResourcePath(romPathKey).toStdString(),
                          getResourcePath(upperRomPathKey).toStdString(),
                          getResourcePath(lowerRomPathKey).toStdString(),
                          getResourcePath(kpcPathKey).toStdString(),
                          getResourcePath(osDiskPathKey).toStdString());
    bridge.prepare(sampleRate);
    hostMidiClock.prepare(sampleRate);
    // A pending snapshot means this prepare is restoring a saved project,
    // not a fresh boot, so the MIDI IN MODE default must not apply - the
    // restored low RAM already holds whatever mode the user had saved.
    const bool restoringSavedState = pendingMachineState.getSize() > 0;
    if (machineSink.isReady() && restoringSavedState &&
        machineSink.restoreState(pendingMachineState.getData(),
                                 pendingMachineState.getSize())) {
        bridge.resetTimeline();
        pendingMachineState.reset();
    }
    appliedDefaultMidiInMode = restoringSavedState;
    const juce::File mounted(getResourcePath(mountedDiskPathKey));
    if (machineSink.isReady() && mounted.existsAsFile() &&
        mounted.getFileExtension().equalsIgnoreCase(".iso"))
        machineSink.insertScsiCd(mounted.getFullPathName().toStdString());
    setLatencySamples(eps16::vst3::BandlimitedResampler::latencySamples(sampleRate));
}

bool Eps16PlusProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    /* Hosts negotiate optional sidechain and auxiliary buses by temporarily
       disabling them. Accept every disabled/stereo combination so the VST3
       wrapper can complete that protocol instead of forcing the host to guess
       a fallback layout. AUv2 may additionally require a stereo main input. */
    const auto mainInput = layouts.getChannelSet(true, 0);
    const auto samplingInput = layouts.getChannelSet(true, 1);
    if (layouts.inputBuses.size() != 2 || layouts.outputBuses.size() != 4 ||
        (!mainInput.isDisabled() &&
         mainInput != juce::AudioChannelSet::stereo()) ||
        (!samplingInput.isDisabled() &&
         samplingInput != juce::AudioChannelSet::stereo()) ||
        layouts.getChannelSet(false, 0) != juce::AudioChannelSet::stereo())
        return false;
    for (int bus = 1; bus < 4; ++bus) {
        const auto channels = layouts.getChannelSet(false, bus);
        if (!channels.isDisabled() && channels != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void Eps16PlusProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                      juce::MidiBuffer &midi) {
    juce::ScopedNoDenormals noDenormals;
    const auto samples = buffer.getNumSamples();
    auto samplingInput = getBusBuffer(buffer, true, 1);
    auto mainOutput = getBusBuffer(buffer, false, 0);
    auto bus2Output = getBusBuffer(buffer, false, 1);
    auto bus3Output = getBusBuffer(buffer, false, 2);
    auto aux1Output = getBusBuffer(buffer, false, 3);
    const float *inputLeft = samplingInput.getNumChannels() > 0
        ? samplingInput.getReadPointer(0) : nullptr;
    const float *inputRight = samplingInput.getNumChannels() > 1
        ? samplingInput.getReadPointer(1) : inputLeft;
    std::size_t eventCount = 0;
    if (const auto *playHead = getPlayHead()) {
        if (const auto position = playHead->getPosition()) {
            const auto bpm = position->getBpm();
            const auto ppq = position->getPpqPosition();
            if (bpm && ppq)
                eventCount += hostMidiClock.generate(
                    true, position->getIsPlaying(), *bpm, *ppq, samples,
                    midiEvents.data(), midiEvents.size());
        }
    }
    std::size_t sysExInputCount = 0;
    for (const auto metadata : midi) {
        if (eventCount == midiEvents.size()) break;
        const auto message = metadata.getMessage();
        const auto *raw = message.getRawData();
        const auto length = message.getRawDataSize();
        if (length < 1) continue;
        if (message.isSysEx()) {
            if (sysExInputCount == sysExInput.size() ||
                static_cast<std::size_t>(length) > maximumInputSysExBytes)
                continue;
            auto &storage = sysExInput[sysExInputCount++];
            std::copy_n(raw, length, storage.begin());
            midiEvents[eventCount++] = {
                juce::jlimit(0, samples, metadata.samplePosition),
                0, 0, 0, storage.data(), static_cast<std::size_t>(length)};
            continue;
        }
        midiEvents[eventCount++] = {
            juce::jlimit(0, samples, metadata.samplePosition), raw[0],
            static_cast<std::uint8_t>(length > 1 ? raw[1] : 0),
            static_cast<std::uint8_t>(length > 2 ? raw[2] : 0)};
    }
    std::stable_sort(midiEvents.begin(), midiEvents.begin() + eventCount,
                     [](const auto &left, const auto &right) {
                         return left.sampleOffset < right.sampleOffset;
                     });

    std::size_t sysExOutputCount = 0;
    /* If the host has none of Bus 2/Bus 3/Aux 1 enabled, emulate a machine
       with no OEX expander fitted: render those three outputs into scratch
       (preallocated in prepareToPlay, never resized here) and sum all of
       them into Main below, so a voice assigned to any EPS output is still
       audible. If the host has enabled any of them, keep exactly the
       existing per-bus behaviour: each enabled bus is routed discretely and
       any bus still disabled is discarded, Aux 1 included. */
    const bool anyOexBusEnabled = bus2Output.getNumChannels() > 0 ||
                                  bus3Output.getNumChannels() > 0 ||
                                  aux1Output.getNumChannels() > 0;
    // The output stage's Bus 2/Bus 3/Aux conversion only differs from
    // Main's when no expander is present; keep it in step with the same
    // host bus state this fold-down already uses, every block.
    machineSink.setOexPresent(anyOexBusEnabled);
    float *bus2Left = nullptr;
    float *bus2Right = nullptr;
    float *bus3Left = nullptr;
    float *bus3Right = nullptr;
    float *aux1Left = nullptr;
    float *aux1Right = nullptr;
    if (anyOexBusEnabled) {
        bus2Left = bus2Output.getNumChannels() > 0
            ? bus2Output.getWritePointer(0) : nullptr;
        bus2Right = bus2Output.getNumChannels() > 1
            ? bus2Output.getWritePointer(1) : nullptr;
        bus3Left = bus3Output.getNumChannels() > 0
            ? bus3Output.getWritePointer(0) : nullptr;
        bus3Right = bus3Output.getNumChannels() > 1
            ? bus3Output.getWritePointer(1) : nullptr;
        aux1Left = aux1Output.getNumChannels() > 0
            ? aux1Output.getWritePointer(0) : nullptr;
        aux1Right = aux1Output.getNumChannels() > 1
            ? aux1Output.getWritePointer(1) : nullptr;
    } else {
        bus2Left = oexScratchBuffer.getWritePointer(0);
        bus2Right = oexScratchBuffer.getWritePointer(1);
        bus3Left = oexScratchBuffer.getWritePointer(2);
        bus3Right = oexScratchBuffer.getWritePointer(3);
        aux1Left = oexScratchBuffer.getWritePointer(4);
        aux1Right = oexScratchBuffer.getWritePointer(5);
    }
    std::array<float *, 8> outputChannels{
        mainOutput.getWritePointer(0), mainOutput.getWritePointer(1),
        bus2Left, bus2Right, bus3Left, bus3Right, aux1Left, aux1Right
    };
    bridge.process(inputLeft, inputRight, outputChannels.data(),
                   outputChannels.size(), samples, midiEvents.data(),
                   eventCount, sysExOutput.data(), sysExOutput.size(),
                   &sysExOutputCount);
    if (!appliedDefaultMidiInMode && machineSink.isReady() &&
        machineSink.cyclesSinceBoot() >= bootCompleteCpuCycles) {
        machineSink.applyDefaultMidiInMode();
        appliedDefaultMidiInMode = true;
    }
    if (!anyOexBusEnabled) {
        auto *mainLeft = mainOutput.getWritePointer(0);
        auto *mainRight = mainOutput.getWritePointer(1);
        for (int sample = 0; sample < samples; ++sample) {
            mainLeft[sample] += bus2Left[sample] + bus3Left[sample] +
                                aux1Left[sample];
            mainRight[sample] += bus2Right[sample] + bus3Right[sample] +
                                 aux1Right[sample];
        }
    }
    midi.clear();
    for (std::size_t index = 0; index < sysExOutputCount; ++index) {
        const auto &event = sysExOutput[index];
        midi.addEvent(juce::MidiMessage(event.bytes.data(),
                                       static_cast<int>(event.size)),
                      event.sampleOffset);
    }
}

juce::AudioProcessorEditor *Eps16PlusProcessor::createEditor() {
    return new Eps16PanelEditor(*this);
}

void Eps16PlusProcessor::getStateInformation(juce::MemoryBlock &destination) {
    const juce::ScopedLock lock(getCallbackLock());
    const auto xmlText = state.toXmlString();
    const auto machine = machineSink.captureState();
    juce::MemoryOutputStream output(destination, false);
    output.writeInt(static_cast<int>(vstStateMagic));
    output.writeInt(static_cast<int>(vstStateVersion));
    output.writeInt64(static_cast<juce::int64>(xmlText.getNumBytesAsUTF8()));
    output.writeInt64(static_cast<juce::int64>(machine.size()));
    output.write(xmlText.toRawUTF8(), xmlText.getNumBytesAsUTF8());
    if (!machine.empty()) output.write(machine.data(), machine.size());
}

bool Eps16PlusProcessor::restoreMachineSnapshot(const void *data,
                                                std::size_t size) {
    const juce::ScopedLock lock(getCallbackLock());
    if (!machineSink.restoreState(data, size)) return false;
    bridge.resetTimeline();
    hostMidiClock.reset();
    return true;
}

void Eps16PlusProcessor::setStateInformation(const void *data, int size) {
    juce::ValueTree restoredTree;
    juce::MemoryBlock restoredMachine;
    bool recognizedContainer = false;
    if (data && size >= 24) {
        juce::MemoryInputStream input(data, static_cast<std::size_t>(size), false);
        const auto magic = static_cast<std::uint32_t>(input.readInt());
        const auto version = static_cast<std::uint32_t>(input.readInt());
        const auto xmlSize = input.readInt64();
        const auto machineSize = input.readInt64();
        const auto remaining = static_cast<juce::int64>(size) - 24;
        recognizedContainer = magic == vstStateMagic;
        if (magic == vstStateMagic && version == vstStateVersion &&
            xmlSize >= 0 && machineSize >= 0 &&
            xmlSize + machineSize == remaining && xmlSize <= 1024 * 1024 &&
            machineSize <= 128 * 1024 * 1024) {
            juce::MemoryBlock xmlData(static_cast<std::size_t>(xmlSize) + 1, true);
            if (input.read(xmlData.getData(), static_cast<int>(xmlSize)) == xmlSize) {
                const auto xml = juce::parseXML(juce::String::fromUTF8(
                    static_cast<const char *>(xmlData.getData()),
                    static_cast<int>(xmlSize)));
                if (xml) restoredTree = juce::ValueTree::fromXml(*xml);
                restoredMachine.setSize(static_cast<std::size_t>(machineSize));
                if (machineSize > 0 &&
                    input.read(restoredMachine.getData(),
                               static_cast<int>(machineSize)) != machineSize)
                    restoredMachine.reset();
            }
        }
    }
    if (!recognizedContainer)
        if (const auto xml = getXmlFromBinary(data, size))
            restoredTree = juce::ValueTree::fromXml(*xml);

    if (!restoredTree.hasType(state.getType())) return;
    const juce::ScopedLock lock(getCallbackLock());
    state = restoredTree;
    refreshResourcePaths();
    pendingMachineState = restoredMachine;
    // A machine snapshot arriving here means this load is restoring a saved
    // project, not a fresh boot; keep the MIDI IN MODE default from firing
    // in processBlock() once the restored state (or this pending one) takes
    // hold, so a saved MULTI/MONO/etc. selection survives the load.
    if (pendingMachineState.getSize() > 0) appliedDefaultMidiInMode = true;
    if (machineSink.isReady() && pendingMachineState.getSize() > 0) {
        bridge.resetTimeline();
        hostMidiClock.reset();
        if (machineSink.restoreState(pendingMachineState.getData(),
                                     pendingMachineState.getSize()))
            pendingMachineState.reset();
    }
}

void Eps16PlusProcessor::setResourcePath(const juce::Identifier &key,
                                         const juce::String &path) {
    state.setProperty(key, path, nullptr);
}

juce::String Eps16PlusProcessor::getResourcePath(const juce::Identifier &key) const {
    return state.getProperty(key).toString();
}

bool Eps16PlusProcessor::insertOsDisk() {
    refreshResourcePaths();
    const juce::File diskFile(getResourcePath(osDiskPathKey));
    if (!diskFile.existsAsFile()) return false;
    const juce::ScopedLock lock(getCallbackLock());
    if (!machineSink.insertDisk(diskFile.getFullPathName().toStdString(),
                                "OS disk"))
        return false;
    state.setProperty(mountedDiskPathKey, diskFile.getFullPathName(), nullptr);
    state.setProperty(blankDiskMountedKey, false, nullptr);
    return true;
}

bool Eps16PlusProcessor::insertDisk(const juce::File &diskFile) {
    if (!diskFile.existsAsFile()) return false;
    const auto extension = diskFile.getFileExtension().toLowerCase();
    if (extension != ".efe" && extension != ".img" && extension != ".hfe" &&
        extension != ".iso")
        return false;
    const juce::ScopedLock lock(getCallbackLock());
    const bool inserted = extension == ".iso"
        ? machineSink.insertScsiCd(diskFile.getFullPathName().toStdString())
        : machineSink.insertDisk(diskFile.getFullPathName().toStdString(),
                                 extension == ".efe" ? "EFE file" : "Disk");
    if (!inserted)
        return false;
    state.setProperty(mountedDiskPathKey, diskFile.getFullPathName(), nullptr);
    state.setProperty(blankDiskMountedKey, false, nullptr);
    return true;
}

bool Eps16PlusProcessor::createBlankDisk() {
    const juce::ScopedLock lock(getCallbackLock());
    if (!machineSink.createBlankDisk()) return false;
    state.setProperty(mountedDiskPathKey, juce::String(), nullptr);
    state.setProperty(blankDiskMountedKey, true, nullptr);
    return true;
}

bool Eps16PlusProcessor::blankDiskMounted() const {
    return static_cast<bool>(state.getProperty(blankDiskMountedKey, false));
}

bool Eps16PlusProcessor::saveDisk(const juce::File &diskFile) {
    auto output = diskFile;
    auto extension = output.getFileExtension().toLowerCase();
    if (extension != ".img" && extension != ".hfe") {
        output = output.withFileExtension(".img");
        extension = ".img";
    }
    const juce::ScopedLock lock(getCallbackLock());
    if (!machineSink.saveDisk(output.getFullPathName().toStdString(),
                              extension == ".hfe"))
        return false;
    state.setProperty(mountedDiskPathKey, output.getFullPathName(), nullptr);
    state.setProperty(blankDiskMountedKey, false, nullptr);
    return true;
}

juce::File Eps16PlusProcessor::defaultResourceDirectory() {
    const auto executable = moduleFile();
    if (!executable.existsAsFile()) return {};
    const auto bundle = executable.getParentDirectory()  // MacOS
                                  .getParentDirectory()  // Contents
                                  .getParentDirectory(); // *.vst3
    return bundle.getParentDirectory().getChildFile("EPS_files");
}

juce::Array<juce::File> Eps16PlusProcessor::resourceSearchDirectories() {
    juce::Array<juce::File> directories;
    const auto addDirectory = [&directories](const juce::File &directory) {
        if (directory.isDirectory() && !directories.contains(directory))
            directories.add(directory);
    };
    addDirectory(defaultResourceDirectory());
#if JUCE_MAC
    addDirectory(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                     .getChildFile("Library/Audio/Plug-Ins/VST3/EPS_files"));
    addDirectory(juce::File("/Library/Audio/Plug-Ins/VST3/EPS_files"));
#elif JUCE_WINDOWS
    addDirectory(juce::File::getSpecialLocation(
                     juce::File::commonApplicationDataDirectory)
                     .getChildFile("VST3/EPS_files"));
#endif
    return directories;
}

void Eps16PlusProcessor::refreshResourcePaths() {
    const auto directories = resourceSearchDirectories();
    if (directories.isEmpty()) return;

    auto discover = [this, &directories](
                        const juce::Identifier &key,
                        std::initializer_list<const char *> names,
                        std::int64_t fallbackSize,
                        const juce::String &fallbackPattern) {
        const juce::File selected(getResourcePath(key));
        if (selected.existsAsFile()) return;
        for (const auto &directory : directories) {
            const auto found = firstExisting(directory, names);
            if (found.existsAsFile()) {
                setResourcePath(key, found.getFullPathName());
                return;
            }
        }
        if (fallbackSize <= 0) return;
        for (const auto &directory : directories) {
            const auto found = firstFileWithSize(directory, fallbackPattern,
                                                 fallbackSize);
            if (found.existsAsFile()) {
                setResourcePath(key, found.getFullPathName());
                return;
            }
        }
    };

    const juce::File selectedRom(getResourcePath(romPathKey));
    const juce::File selectedUpper(getResourcePath(upperRomPathKey));
    const juce::File selectedLower(getResourcePath(lowerRomPathKey));
    if (!selectedRom.existsAsFile() &&
        isKnownSplitRom(selectedUpper, selectedLower)) {
        setResourcePath(romPathKey, {});
    } else if (!selectedRom.existsAsFile()) {
        setResourcePath(upperRomPathKey, {});
        setResourcePath(lowerRomPathKey, {});
        for (const auto &directory : directories) {
            auto combined = firstExisting(directory, {"eps16plus-rom.bin"});
            if (!combined.existsAsFile())
                combined = firstFileWithSize(directory, "*.bin;*.rom", 131072);
            if (combined.existsAsFile()) {
                setResourcePath(romPathKey, combined.getFullPathName());
                break;
            }
            const auto split = findKnownSplitRom(directory);
            if (!split.upper.existsAsFile() || !split.lower.existsAsFile())
                continue;
            setResourcePath(romPathKey, {});
            setResourcePath(upperRomPathKey, split.upper.getFullPathName());
            setResourcePath(lowerRomPathKey, split.lower.getFullPathName());
            break;
        }
    }
    discover(kpcPathKey,
             {"eps16plus-kpc.bin", "Ensoniq EPS KPC2 v2.33 27c256.BIN"},
             32768, "*.bin;*.rom");
    discover(osDiskPathKey, {"EPS130OS.img", "EPS130OS.hfe"}, 819200, "*.img");
    if (!juce::File(getResourcePath(osDiskPathKey)).existsAsFile()) {
        for (const auto &directory : directories) {
            auto hfeFiles = directory.findChildFiles(juce::File::findFiles,
                                                      false, "*.hfe;*.HFE");
            hfeFiles.sort();
            if (!hfeFiles.isEmpty()) {
                setResourcePath(osDiskPathKey,
                                hfeFiles.getFirst().getFullPathName());
                break;
            }
        }
    }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
    return new Eps16PlusProcessor();
}
