// Checks the built Probe plug-in the way a DAW uses it: loads SessionScopeProbe.vst3, restores a
// saved state that names the SessionScope Sensor as the effect inside, tells it its track, runs
// audio through it, saves the state again, and looks in the probes' shared table.
//
//   test_probe_bundle <path to SessionScopeProbe.vst3> <path to SessionScope.vst3>
//
// The "DAW" here is our own HostedVst3 class holding the Probe, so there are two layers:
//   this test  ->  HostedVst3 (table: a test one)  ->  Probe  ->  HostedVst3 (table: the real one)  ->  Sensor
//
// Run by `ctest --preset windows-plugins` (native/CMakeLists.txt). Exit code 0 = passed; any other
// number says which step failed.
#include "hosted_vst3.h"
#include "pluginterfaces/vst/ivstchannelcontextinfo.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "public.sdk/source/common/memorystream.h"

#include <array>
#include <cstring>
#include <iostream>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// One "blob" of the Probe's state: 4 bytes of length, then the bytes (layout: sessionscope_probe.cpp).
void write_field(MemoryStream& state, std::string field) {
    uint32 length = static_cast<uint32>(field.size());
    state.write(&length, sizeof(length), nullptr);   // this test runs on little-endian processors only, like the layout
    if (length) state.write(field.data(), length, nullptr);
}

// Sets the Probe up as a DAW would, runs one block of 4 samples and one empty call through it,
// and checks the audio came out unchanged (the Sensor inside passes audio through).
bool process_sensor(sessionscope::HostedVst3& probe) {
    auto* component = probe.component();
    auto* processor = probe.processor();
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    ProcessSetup setup{kRealtime, kSample32, 4, 48000};
    // With the Sensor inside, the Probe must answer as the Sensor does: an endless tail (it must
    // never be put to sleep) and no 64-bit audio.
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
    // The outer layer reports into a table of its own; only the Probe inside uses the real table.
    const auto table_name = "SessionScope_test_outer_probe_" + std::to_string(GetCurrentProcessId());
    sessionscope::HostedVst3 probe(table_name.c_str());
    const FUID probe_id(0x8B2E807B, 0xC9014C31, 0xA9788B53, 0xB4EA30F2);    // the Probe's class id
    const FUID sensor_id(0x5E55F0A1, 0x7C0D4B6E, 0x9A2E1D43, 0x0B5C8E77);   // the Sensor's class id
    const VST3::UID probe_uid(probe_id.toTUID());
    std::string error;
    if (!probe.load(argv[1], probe_uid, error)) {
        std::cerr << error << '\n';
        return 2;
    }

    // REAPER picks 32 or 64 bit, sets the plug-in up and switches it on BEFORE it hands over the
    // saved state. So the empty Probe must say "32 bit only" (every effect can do that), and must
    // take a state while switched on.
    auto* processor = probe.processor();
    if (processor->canProcessSampleSize(kSample32) != kResultTrue ||
        processor->canProcessSampleSize(kSample64) == kResultTrue)
        return 8;
    ProcessSetup initial_setup{kRealtime, kSample32, 4, 48000};
    if (probe.setup(initial_setup) != kResultOk || probe.component()->setActive(true) != kResultOk)
        return 9;

    // The DAW names the track before the effect is restored: the name must be kept for later.
    FUnknownPtr<ChannelContext::IInfoListener> listener(probe.component());
    if (!listener) return 10;
    auto attributes = HostAttributeList::make();   // the SDK's own list of named values
    attributes->setString(ChannelContext::kChannelNameKey, u"Lead Vox");
    attributes->setInt(ChannelContext::kChannelIndexKey, 3);
    if (listener->setChannelContextInfos(attributes) != kResultOk) return 11;

    // A saved state: version 1, the Sensor's path and id, no state of its own (it has none).
    MemoryStream state;
    uint32 version = 1;
    state.write(&version, sizeof(version), nullptr);
    write_field(state, argv[2]);
    write_field(state, VST3::UID(sensor_id.toTUID()).toString());
    write_field(state, {});
    write_field(state, {});
    state.seek(0, IBStream::kIBSeekSet, nullptr);   // back to the start, for reading
    if (probe.component()->setState(&state) != kResultOk) return 3;
    // The DAW restarts the plug-in, and audio goes through the Sensor inside.
    if (probe.component()->setActive(false) != kResultOk || !process_sensor(probe)) return 4;

    // Saving must give back exactly what was loaded.
    MemoryStream saved;
    if (probe.component()->getState(&saved) != kResultOk ||
        saved.getSize() != state.getSize() || std::memcmp(saved.getData(), state.getData(), state.getSize()) != 0)
        return 5;

    // The real table must hold the Sensor, on track "Lead Vox" at position 3, in this process,
    // with one block timed (4 samples at 48 kHz = 83333 nanoseconds).
    const auto* table = sessionscope::probes::open_table();
    if (!table) return 6;
    for (const auto& slot : table->slot) {
        if (slot.owner.load() != 0 && slot.process_id == GetCurrentProcessId() &&
            std::string(slot.plugin) == "SessionScope Sensor" && std::string(slot.track) == "Lead Vox" &&
            slot.track_order == 3 && slot.blocks.load() == 1 && slot.budget_ns.load() == 83333)
            return 0;
    }
    return 7;
}
