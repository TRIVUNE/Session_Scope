// Exercises the Probe's hosting path against the built SessionScope Sensor VST3 bundle.
#include "hosted_vst3.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <array>
#include <iostream>

using namespace Steinberg;
using namespace Steinberg::Vst;

int main(int argc, char** argv) {
    if (argc != 2) return 1;

    const auto table_name = "SessionScope_test_vst3_probe_" + std::to_string(GetCurrentProcessId());
    sessionscope::HostedVst3 effect(table_name.c_str());
    const FUID sensor_id(0x5E55F0A1, 0x7C0D4B6E, 0x9A2E1D43, 0x0B5C8E77);
    std::string error;
    if (effect.load(argv[1], VST3::UID{}, error) || error.empty()) return 2;
    if (!effect.load(argv[1], VST3::UID(sensor_id.toTUID()), error)) {
        std::cerr << error << '\n';
        return 3;
    }

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
    if (result != kResultOk || flush != kResultOk || left != out_left || right != out_right) return 5;

    const auto* table = sessionscope::probes::open_table(table_name.c_str());
    if (!table) return 6;
    for (const auto& slot : table->slot) {
        if (slot.owner.load() != 0 && std::string(slot.plugin) == "SessionScope Sensor" &&
            slot.blocks.load() == 1 && slot.budget_ns.load() == 83333)
            return 0;
    }
    return 7;
}
