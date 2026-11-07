// SessionScope Sensor (Audio Unit, macOS): goes on the master track. Audio passes through unchanged.
//
// This file only translates the Audio Unit API into the shared sensor core: each audio block
// becomes one BlockInfo, which sensor_ring.h writes into shared memory for the HUD. The ring
// layout and the real-time rules are documented in that header; the VST3 and CLAP sensors are the
// same wrapper for their formats.
//
// Built on Apple's AudioUnitSDK (AUEffectBase). An Audio Unit asks the host for the transport
// through callbacks rather than being handed it; those callbacks are made for the render thread.
// Info.plist (next to this file) registers it as effect 'aufx', subtype 'SScp', maker 'Trvn'.

#include <AudioUnitSDK/AUEffectBase.h>

#include <cmath>
#include <cstring>

#include "../sensor_core/sensor_ring.h"

namespace {

class SensorAU : public ausdk::AUEffectBase {
public:
    // `true`: the host may give us the same buffer for input and output.
    explicit SensorAU(AudioComponentInstance instance) : AUEffectBase(instance, true), writer_(this) {}

    // A sensor must keep running through silence: a long tail stops hosts from skipping it. An
    // hour, not "forever": hosts turn the tail into a sample count, and an hour at 192 kHz still
    // fits a 32-bit one (6.9e8 < 2.1e9).
    bool SupportsTail() AUSDK_RTSAFE override { return true; }
    Float64 GetTailTime() AUSDK_RTSAFE override { return 3600.0; }

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags &, const AudioBufferList &in, AudioBufferList &out,
                                UInt32 frames) AUSDK_RTSAFE override {
        if (frames == 0) return noErr;   // not an audio block

        const double t0 = sessionscope::now_seconds();
        pass_audio_through(in, out, frames);
        writer_.write(describe_block(t0, frames));
        writer_.set_cost((sessionscope::now_seconds() - t0) * 1e6);   // reported with the next record
        return noErr;
    }

private:
    static void pass_audio_through(const AudioBufferList &in, AudioBufferList &out, UInt32 frames) {
        const UInt32 buffers = in.mNumberBuffers < out.mNumberBuffers ? in.mNumberBuffers : out.mNumberBuffers;
        for (UInt32 b = 0; b < buffers; ++b) {
            const AudioBuffer &source = in.mBuffers[b];
            AudioBuffer &target = out.mBuffers[b];
            if (!source.mData || !target.mData || source.mData == target.mData) continue;
            const UInt32 bytes = frames * sizeof(float) * source.mNumberChannels;
            std::memcpy(target.mData, source.mData, bytes < target.mDataByteSize ? bytes : target.mDataByteSize);
        }
    }

    // What the host told us about this block. Anything it did not tell us stays NaN, never a made-up
    // value, except the time signature (4/4) and the play position (0), as in the other sensors.
    sessionscope::BlockInfo describe_block(double t0, UInt32 frames) {
        const double unknown = std::nan("");
        const double sample_rate = GetSampleRate();

        sessionscope::BlockInfo block{};
        block.t0 = t0;
        block.frames = frames;
        block.srate = sample_rate;
        block.play_state = 0;
        block.play_pos = 0;
        block.qn = unknown;
        block.tempo = unknown;
        block.ts_num = 4;
        block.ts_den = 4;
        block.bar_qn = unknown;

        Boolean playing = false, changed = false, cycling = false;
        Float64 sample_in_timeline = 0, cycle_start = 0, cycle_end = 0;
        if (CallHostTransportState(&playing, &changed, &sample_in_timeline, &cycling, &cycle_start, &cycle_end) == noErr) {
            block.play_state = playing ? 1 : 0;   // an Audio Unit is not told whether the host records
            if (sample_rate > 0) block.play_pos = sample_in_timeline / sample_rate;
        }

        Float64 beat = 0, tempo = 0;
        if (CallHostBeatAndTempo(&beat, &tempo) == noErr) {
            block.qn = beat;   // Audio Unit beats are quarter notes
            block.tempo = tempo;
        }

        UInt32 samples_to_next_beat = 0, denominator = 0;
        Float32 numerator = 0;
        Float64 bar_start = 0;
        if (CallHostMusicalTimeLocation(&samples_to_next_beat, &numerator, &denominator, &bar_start) == noErr &&
            numerator > 0 && denominator > 0) {
            block.ts_num = numerator;
            block.ts_den = denominator;
            // The host's own bar line: keeps bar numbers right across time-signature changes.
            block.bar_qn = bar_start;
        }
        return block;
    }

    sessionscope::RingWriter writer_;
};

}  // namespace

// The function Info.plist names as factoryFunction: SensorAUFactory.
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, SensorAU)
