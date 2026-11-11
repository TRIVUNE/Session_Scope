// SessionScope Sensor (VST3): goes on the master track. Audio passes through unchanged.
//
// This file only translates VST3 into the shared sensor core: each audio block becomes one
// BlockInfo, which sensor_ring.h writes into shared memory for the HUD. The ring layout and the
// real-time rules are documented in that header.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../sensor_core/sensor_ring.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// Copy input to output. Hosts may hand us the same buffer for both, or no buffers at all.
void pass_audio_through(ProcessData &data) {
    if (data.numInputs < 1 || data.numOutputs < 1) return;

    AudioBusBuffers &in = data.inputs[0];
    AudioBusBuffers &out = data.outputs[0];
    if (!in.channelBuffers32 || !out.channelBuffers32) return;

    const int32 channels = in.numChannels < out.numChannels ? in.numChannels : out.numChannels;
    for (int32 c = 0; c < channels; ++c) {
        if (out.channelBuffers32[c] != in.channelBuffers32[c])
            std::memcpy(out.channelBuffers32[c], in.channelBuffers32[c], sizeof(float) * data.numSamples);
    }
    out.silenceFlags = in.silenceFlags;
}

// What the host told us about this block. Anything it did not tell us stays NaN, never a made-up
// value, except the time signature, which falls back to 4/4.
sessionscope::BlockInfo describe_block(const ProcessData &data, double t0, double fallback_sample_rate) {
    const ProcessContext *context = data.processContext;
    const uint32 state = context ? context->state : 0;
    const double sample_rate = context && context->sampleRate > 0 ? context->sampleRate : fallback_sample_rate;
    const double unknown = std::nan("");
    const bool has_time_signature = state & ProcessContext::kTimeSigValid;

    sessionscope::BlockInfo block{};
    block.t0 = t0;
    block.frames = data.numSamples;
    block.srate = sample_rate;
    block.play_state = (state & ProcessContext::kRecording) ? 5 : (state & ProcessContext::kPlaying) ? 1 : 0;
    block.play_pos = context ? static_cast<double>(context->projectTimeSamples) / sample_rate : 0;
    block.qn = (state & ProcessContext::kProjectTimeMusicValid) ? context->projectTimeMusic : unknown;
    block.tempo = (state & ProcessContext::kTempoValid) ? context->tempo : unknown;
    block.ts_num = has_time_signature ? context->timeSigNumerator : 4;
    block.ts_den = has_time_signature ? context->timeSigDenominator : 4;
    // The host's own bar line: keeps bar numbers right across time-signature changes.
    block.bar_qn = (state & ProcessContext::kBarPositionValid) ? context->barPositionMusic : unknown;
    return block;
}

class SensorProcessor : public SingleComponentEffect {
public:
    static FUnknown *createInstance(void *) { return static_cast<IAudioProcessor *>(new SensorProcessor); }

    SensorProcessor() : writer_{this} {}

    tresult PLUGIN_API initialize(FUnknown *context) SMTG_OVERRIDE {
        tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Audio In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Audio Out"), SpeakerArr::kStereo);
        return kResultOk;
    }

    // Any layout works as long as input and output match, so surround masters can load the sensor.
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns, SpeakerArrangement *outputs,
                                          int32 numOuts) SMTG_OVERRIDE {
        const bool one_matching_bus = numIns == 1 && numOuts == 1 && inputs[0] == outputs[0];
        if (one_matching_bus && SpeakerArr::getChannelCount(inputs[0]) > 0)
            return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
        return kResultFalse;
    }

    // A sensor must keep running through silence. Without this, some hosts treat the effect as
    // having no tail and stop calling it once the input goes quiet.
    uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE { return kInfiniteTail; }

    // Saving and loading: the sensor has no settings, so there is nothing to write or read. It
    // still answers "done" for both halves of a VST3's state (the audio half and the window
    // half): the base class answers "not implemented", which Ableton Live writes into its log as
    // an error every time it saves ("couldn't get processor state of SessionScope Sensor").
    tresult PLUGIN_API getState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API getEditorState(IBStream *) SMTG_OVERRIDE { return kResultOk; }
    tresult PLUGIN_API setEditorState(IBStream *) SMTG_OVERRIDE { return kResultOk; }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) SMTG_OVERRIDE {
        sample_rate_ = setup.sampleRate;
        return SingleComponentEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API process(ProcessData &data) SMTG_OVERRIDE {
        // Hosts may call with no samples just to flush parameters. That is not an audio block:
        // recording it would corrupt the timing of the next real one.
        if (data.numSamples <= 0) return kResultOk;

        const double t0 = sessionscope::now_seconds();
        pass_audio_through(data);
        writer_.write(describe_block(data, t0, sample_rate_));
        writer_.set_cost((sessionscope::now_seconds() - t0) * 1e6);   // reported with the next record
        return kResultOk;
    }

private:
    sessionscope::RingWriter writer_;
    double sample_rate_ = 48000;
};

// Class id of this plug-in: generated once, never to change (hosts remember it).
const FUID SensorUID(0x5E55F0A1, 0x7C0D4B6E, 0x9A2E1D43, 0x0B5C8E77);

}  // namespace

BEGIN_FACTORY_DEF("TRIVUNE", "https://github.com/TRIVUNE/Session-Scope", "mailto:theodorpetrea643@gmail.com")

DEF_CLASS2(INLINE_UID_FROM_FUID(SensorUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "SessionScope Sensor",
           0, PlugType::kFxAnalyzer, "0.3.0", kVstVersionString, SensorProcessor::createInstance)

END_FACTORY

// Called by the SDK's DLL entry points when the host loads and unloads the module. Nothing to set up.
bool InitModule() { return true; }
bool DeinitModule() { return true; }
