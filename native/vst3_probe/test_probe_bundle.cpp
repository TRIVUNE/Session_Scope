// Loads the built Probe as a host would, restores a selected effect, and checks audio and timing.
#include "hosted_vst3.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/common/memorystream.h"

#include <array>
#include <cstring>
#include <iostream>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void write_field(MemoryStream& state, std::string field) {
    uint32 length = static_cast<uint32>(field.size());
    state.write(&length, sizeof(length), nullptr);
    if (length) state.write(field.data(), length, nullptr);
}

bool process_sensor(sessionscope::HostedVst3& probe) {
    auto* component = probe.component();
    auto* processor = probe.processor();
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    ProcessSetup setup{kRealtime, kSample32, 4, 48000};
    if (processor->getTailSamples() != kInfiniteTail || processor->canProcessSampleSize(kSample64) == kResultTrue ||
        processor->setBusArrangements(&stereo, 1, &stereo, 1) != kResultOk ||
        component->activateBus(kAudio, kInput, 0, true) != kResultOk ||
        component->activateBus(kAudio, kOutput, 0, true) != kResultOk ||
        probe.setup(setup) != kResultOk || component->setActive(true) != kResultOk)
        return false;
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
    const auto result = probe.process(data);
    data.numSamples = 0;
    const auto flush = probe.process(data);

    processor->setProcessing(false);
    component->setActive(false);
    return result == kResultOk && flush == kResultOk && left == out_left && right == out_right;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const auto table_name = "SessionScope_test_outer_probe_" + std::to_string(GetCurrentProcessId());
    sessionscope::HostedVst3 probe(table_name.c_str());
    const FUID probe_id(0x8B2E807B, 0xC9014C31, 0xA9788B53, 0xB4EA30F2);
    const FUID sensor_id(0x5E55F0A1, 0x7C0D4B6E, 0x9A2E1D43, 0x0B5C8E77);
    const VST3::UID probe_uid(probe_id.toTUID());
    std::string error;
    if (!probe.load(argv[1], probe_uid, error)) {
        std::cerr << error << '\n';
        return 2;
    }

    // REAPER chooses the initial processing format before supplying saved state.
    auto* processor = probe.processor();
    if (processor->canProcessSampleSize(kSample32) != kResultTrue ||
        processor->canProcessSampleSize(kSample64) == kResultTrue)
        return 8;
    ProcessSetup initial_setup{kRealtime, kSample32, 4, 48000};
    if (probe.setup(initial_setup) != kResultOk || probe.component()->setActive(true) != kResultOk)
        return 9;

    MemoryStream state;
    uint32 version = 1;
    state.write(&version, sizeof(version), nullptr);
    write_field(state, argv[2]);
    write_field(state, VST3::UID(sensor_id.toTUID()).toString());
    write_field(state, {});
    write_field(state, {});
    state.seek(0, IBStream::kIBSeekSet, nullptr);
    if (probe.component()->setState(&state) != kResultOk) return 3;
    if (probe.component()->setActive(false) != kResultOk || !process_sensor(probe)) return 4;

    MemoryStream saved;
    if (probe.component()->getState(&saved) != kResultOk ||
        saved.getSize() != state.getSize() || std::memcmp(saved.getData(), state.getData(), state.getSize()) != 0)
        return 5;

    const auto* table = sessionscope::probes::open_table();
    if (!table) return 6;
    for (const auto& slot : table->slot) {
        if (slot.owner.load() != 0 && slot.process_id == GetCurrentProcessId() &&
            std::string(slot.plugin) == "SessionScope Sensor" && slot.blocks.load() == 1 &&
            slot.budget_ns.load() == 83333)
            return 0;
    }
    return 7;
}
