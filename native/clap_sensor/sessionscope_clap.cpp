// SessionScope Sensor (CLAP): goes on the master track. Audio passes through unchanged.
//
// This file only translates CLAP into the shared sensor core: each audio block becomes one
// BlockInfo, which sensor_ring.h writes into shared memory for the HUD. The ring layout and the
// real-time rules are documented in that header; the VST3 sensor (native/vst3_sensor) is the
// same wrapper for VST3 hosts.
//
// CLAP is a plain C interface: the host calls through tables of function pointers. Each table
// below is filled with small static functions that forward to the Sensor object.

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include "../sensor_core/sensor_ring.h"

namespace {

// What the host told us about this block. Anything it did not tell us stays NaN, never a made-up
// value, except the time signature, which falls back to 4/4, and the play position, which falls
// back to 0 (the VST3 sensor does the same, so the engine sees both alike).
sessionscope::BlockInfo describe_block(const clap_process_t &process, double t0, double sample_rate) {
    const clap_event_transport_t *transport = process.transport;   // null: the host has no timeline
    const uint32_t flags = transport ? transport->flags : 0;
    const double unknown = std::nan("");
    const double beats = static_cast<double>(CLAP_BEATTIME_FACTOR);
    const double seconds = static_cast<double>(CLAP_SECTIME_FACTOR);
    const bool has_beats = flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    const bool has_time_signature = flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE;

    sessionscope::BlockInfo block{};
    block.t0 = t0;
    block.frames = process.frames_count;
    block.srate = sample_rate;
    block.play_state = (flags & CLAP_TRANSPORT_IS_RECORDING) ? 5 : (flags & CLAP_TRANSPORT_IS_PLAYING) ? 1 : 0;
    block.play_pos = (flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) ? transport->song_pos_seconds / seconds : 0;
    // CLAP's beats are quarter notes, like VST3's projectTimeMusic.
    block.qn = has_beats ? transport->song_pos_beats / beats : unknown;
    block.tempo = (flags & CLAP_TRANSPORT_HAS_TEMPO) ? transport->tempo : unknown;
    block.ts_num = has_time_signature ? transport->tsig_num : 4;
    block.ts_den = has_time_signature ? transport->tsig_denom : 4;
    // The host's own bar line: keeps bar numbers right across time-signature changes.
    block.bar_qn = has_beats ? transport->bar_start / beats : unknown;
    return block;
}

// Copy input to output. Hosts may hand us the same buffer for both (in-place), or no buffers.
void pass_audio_through(const clap_process_t &process) {
    if (process.audio_inputs_count < 1 || process.audio_outputs_count < 1) return;

    const clap_audio_buffer_t &in = process.audio_inputs[0];
    clap_audio_buffer_t &out = process.audio_outputs[0];
    if (!in.data32 || !out.data32) return;

    const uint32_t channels = in.channel_count < out.channel_count ? in.channel_count : out.channel_count;
    for (uint32_t c = 0; c < channels; ++c) {
        if (out.data32[c] != in.data32[c])
            std::memcpy(out.data32[c], in.data32[c], sizeof(float) * process.frames_count);
    }
    out.constant_mask = in.constant_mask;   // a constant (e.g. silent) input stays marked as such
}

const char *const FEATURES[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_ANALYZER,
    CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor_t DESCRIPTOR = {
    CLAP_VERSION_INIT,
    "com.trivune.sessionscope.sensor",   // never to change: hosts remember plug-ins by this id
    "SessionScope Sensor",
    "TRIVUNE",
    "https://github.com/TRIVUNE/Session-Scope",
    "",
    "",
    "0.3.0",
    "Measures every audio block for the SessionScope HUD. Audio passes through unchanged.",
    FEATURES,
};

class Sensor {
public:
    Sensor() : writer_(this) {
        plugin_.desc = &DESCRIPTOR;
        plugin_.plugin_data = this;
        plugin_.init = [](const clap_plugin_t *) { return true; };
        plugin_.destroy = [](const clap_plugin_t *p) { delete self(p); };
        plugin_.activate = [](const clap_plugin_t *p, double sample_rate, uint32_t, uint32_t) {
            self(p)->sample_rate_ = sample_rate;
            return true;
        };
        plugin_.deactivate = [](const clap_plugin_t *) {};
        plugin_.start_processing = [](const clap_plugin_t *) { return true; };
        plugin_.stop_processing = [](const clap_plugin_t *) {};
        plugin_.reset = [](const clap_plugin_t *) {};
        plugin_.process = [](const clap_plugin_t *p, const clap_process_t *process) {
            return self(p)->process(*process);
        };
        plugin_.get_extension = [](const clap_plugin_t *, const char *id) -> const void * {
            if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &AUDIO_PORTS;
            if (std::strcmp(id, CLAP_EXT_TAIL) == 0) return &TAIL;
            return nullptr;
        };
        plugin_.on_main_thread = [](const clap_plugin_t *) {};
    }

    const clap_plugin_t *plugin() const { return &plugin_; }

private:
    static Sensor *self(const clap_plugin_t *p) { return static_cast<Sensor *>(p->plugin_data); }

    clap_process_status process(const clap_process_t &process) {
        // A call with no samples is not an audio block: recording it would corrupt the timing of
        // the next real one.
        if (process.frames_count == 0) return CLAP_PROCESS_CONTINUE;

        const double t0 = sessionscope::now_seconds();
        pass_audio_through(process);
        writer_.write(describe_block(process, t0, sample_rate_));
        writer_.set_cost((sessionscope::now_seconds() - t0) * 1e6);   // reported with the next record
        return CLAP_PROCESS_CONTINUE;   // keep calling us through silence: a sensor must not sleep
    }

    // One stereo input and one stereo output, processed in place when the host wants to.
    static uint32_t port_count(const clap_plugin_t *, bool) { return 1; }
    static bool port_info(const clap_plugin_t *, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
        if (index != 0) return false;
        info->id = 0;
        std::strncpy(info->name, is_input ? "Audio In" : "Audio Out", sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = 0;   // input 0 and output 0 may share buffers
        return true;
    }
    static constexpr clap_plugin_audio_ports_t AUDIO_PORTS = {port_count, port_info};

    // Without an infinite tail, a host may stop calling the effect once the input goes quiet.
    static uint32_t tail_length(const clap_plugin_t *) { return INT32_MAX; }
    static constexpr clap_plugin_tail_t TAIL = {tail_length};

    clap_plugin_t plugin_{};
    sessionscope::RingWriter writer_;
    double sample_rate_ = 48000;
};

// The factory: the host asks how many plug-ins this file holds (one) and creates them by id.
uint32_t plugin_count(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *, uint32_t index) {
    return index == 0 ? &DESCRIPTOR : nullptr;
}

const clap_plugin_t *create_plugin(const clap_plugin_factory_t *, const clap_host_t *host, const char *id) {
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, DESCRIPTOR.id) != 0) return nullptr;
    Sensor *sensor = new (std::nothrow) Sensor();   // creates the shared mapping, off the audio thread
    return sensor ? sensor->plugin() : nullptr;
}

const clap_plugin_factory_t FACTORY = {plugin_count, plugin_descriptor, create_plugin};

}  // namespace

// The one symbol a CLAP host looks for in the file.
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char *) { return true; },   // init: nothing to set up
    []() {},                             // deinit
    [](const char *factory_id) -> const void * {
        return std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &FACTORY : nullptr;
    },
};
