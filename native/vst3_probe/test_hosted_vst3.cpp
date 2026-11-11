// Checks the Probe's hosting core (hosted_vst3.h) on its own, without the Probe plug-in around it:
// it loads the built SessionScope Sensor (a plug-in we know exactly), runs one audio block through
// it, and looks in the probes' shared table for what was recorded.
//
//   test_hosted_vst3 <path to SessionScope.vst3>
//
// Run by `ctest --preset windows-plugins` (native/CMakeLists.txt). Exit code 0 = passed; any other
// number says which step failed.
#include "hosted_vst3.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <array>
#include <iostream>

using namespace Steinberg;
using namespace Steinberg::Vst;

int main(int argc, char** argv) {
    if (argc != 2) return 1;

    // A table of our own, named after this process, so real probes in a running DAW never mix in.
    const auto table_name = "SessionScope_test_vst3_probe_" + std::to_string(GetCurrentProcessId());
    sessionscope::HostedVst3 effect(table_name.c_str());
    effect.set_track("Drums", 2);   // before load(): the name must be kept and published once there is a slot
    const FUID sensor_id(0x5E55F0A1, 0x7C0D4B6E, 0x9A2E1D43, 0x0B5C8E77);   // the Sensor's class id
    std::string error;
    // An id the file doesn't hold must fail, with a reason.
    if (effect.load(argv[1], VST3::UID{}, error) || error.empty()) return 2;
    if (!effect.load(argv[1], VST3::UID(sensor_id.toTUID()), error)) {
        std::cerr << error << '\n';
        return 3;
    }

    // Set the effect up the way a DAW does: stereo in and out, 48 kHz, blocks of 4 samples, switched on.
    auto* component = effect.component();
    auto* processor = effect.processor();
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    ProcessSetup setup{kRealtime, kSample32, 4, 48000};
    if (!effect.controller() || processor->setBusArrangements(&stereo, 1, &stereo, 1) != kResultOk ||
        component->activateBus(kAudio, kInput, 0, true) != kResultOk ||
        component->activateBus(kAudio, kOutput, 0, true) != kResultOk ||
        effect.setup(setup) != kResultOk || component->setActive(true) != kResultOk)
        return 4;
    processor->setProcessing(true);

    // One block of 4 samples, then a call with no samples (parameter changes only: must not be timed).
    std::array<float, 4> left{1, 0.5f, -0.5f, -1}, right{0, -1, 1, 0}, out_left{}, out_right{};
    float* input_channels[]{left.data(), right.data()};
    float* output_channels[]{out_left.data(), out_right.data()};
    AudioBusBuffers input, output;
    input.numChannels = output.numChannels = 2;
    input.channelBuffers32 = input_channels;
    output.channelBuffers32 = output_channels;
    ProcessData data;
    data.numInputs = data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
    data.numSamples = 4;
    const auto result = effect.process(data);
    data.numSamples = 0;
    const auto flush = effect.process(data);

    processor->setProcessing(false);
    component->setActive(false);
    // The Sensor passes audio through unchanged, so the output must equal the input.
    if (result != kResultOk || flush != kResultOk || left != out_left || right != out_right) return 5;

    // The table must now hold one slot: the Sensor on track "Drums", one block timed, lasting
    // 4 samples / 48000 per second = 83333 nanoseconds.
    const auto* table = sessionscope::probes::open_table(table_name.c_str());
    if (!table) return 6;
    bool found = false;
    for (const auto& slot : table->slot) {
        if (slot.owner.load() != 0 && std::string(slot.plugin) == "SessionScope Sensor" &&
            std::string(slot.track) == "Drums" && slot.track_order == 2 &&
            slot.blocks.load() == 1 && slot.budget_ns.load() == 83333)
            found = true;
    }
    if (!found) return 7;

    // The DAW renames the track: the slot follows.
    effect.set_track("Drum bus", 5);
    for (const auto& slot : table->slot) {
        if (slot.owner.load() != 0 && std::string(slot.track) == "Drum bus" && slot.track_order == 5) return 0;
    }
    return 8;
}
