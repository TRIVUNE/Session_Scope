// A tiny CLAP host that checks the CLAP sensor end to end: it loads SessionScope.clap, plays
// audio blocks with a known transport, and reads the records back from the shared ring.
// clap-validator checks that the plug-in follows the CLAP rules; this checks what it measures.
//
//     test_clap_sensor build/native/SessionScope.clap
//
// Uses the real shared ring, so do not run it while a DAW has the sensor loaded.
// Prints "ok" and returns 0 if every check passes.

#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../sensor_core/sensor_ring.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <string>
#include <sys/stat.h>
#endif

using namespace sessionscope;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const clap_plugin_entry_t *load_entry(const char *path) {
#ifdef _WIN32
    HMODULE module = LoadLibraryA(path);
    if (!module) return nullptr;
    return reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(module, "clap_entry"));
#else
    // On macOS a .clap is a bundle (a folder); the code is inside it.
    std::string binary = path;
    struct stat info;
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) binary += "/Contents/MacOS/SessionScope";
    void *module = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) return nullptr;
    return static_cast<const clap_plugin_entry_t *>(dlsym(module, "clap_entry"));
#endif
}

// The host side: no extensions, and requests are ignored.
const clap_host_t HOST = {
    CLAP_VERSION_INIT, nullptr, "SessionScope test host", "TRIVUNE", "", "1.0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

// Empty event lists: the sensor gets no notes or parameter changes.
const clap_input_events_t NO_INPUT_EVENTS = {
    nullptr,
    [](const clap_input_events_t *) -> uint32_t { return 0; },
    [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return nullptr; },
};
const clap_output_events_t NO_OUTPUT_EVENTS = {
    nullptr,
    [](const clap_output_events_t *, const clap_event_header_t *) { return false; },
};

constexpr uint32_t FRAMES = 256;
constexpr double RATE = 48000;
constexpr int BLOCKS = 10;

// Plays one block. Returns the output, so the caller can check the audio passed through.
std::vector<float> play_block(const clap_plugin_t *plugin, const clap_event_transport_t *transport,
                              uint32_t frames, float value) {
    std::vector<float> in_left(FRAMES, value), in_right(FRAMES, -value);
    std::vector<float> out_left(FRAMES, 0), out_right(FRAMES, 0);
    float *in_channels[2] = {in_left.data(), in_right.data()};
    float *out_channels[2] = {out_left.data(), out_right.data()};

    clap_audio_buffer_t input{in_channels, nullptr, 2, 0, 0};
    clap_audio_buffer_t output{out_channels, nullptr, 2, 0, 0};
    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = frames;
    process.transport = transport;
    process.audio_inputs = &input;
    process.audio_outputs = &output;
    process.audio_inputs_count = 1;
    process.audio_outputs_count = 1;
    process.in_events = &NO_INPUT_EVENTS;
    process.out_events = &NO_OUTPUT_EVENTS;
    check(plugin->process(plugin, &process) == CLAP_PROCESS_CONTINUE, "process asks to keep running");

    out_left.insert(out_left.end(), out_right.begin(), out_right.end());
    return out_left;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_clap_sensor PATH_TO_SessionScope.clap\n");
        return 2;
    }

    // A fresh ring: the sensor sets up the header again when it finds none.
    volatile double *ring = shared_ring();
    if (!ring) {
        std::fprintf(stderr, "FAIL: no shared ring\n");
        return 1;
    }
    for (int i = 0; i < HDR; ++i) ring[i] = 0;

    const clap_plugin_entry_t *entry = load_entry(argv[1]);
    if (!entry || !entry->init(argv[1])) {
        std::fprintf(stderr, "FAIL: could not load %s\n", argv[1]);
        return 1;
    }
    const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "one plug-in in the file");
    const clap_plugin_t *plugin = factory->create_plugin(factory, &HOST, "com.trivune.sessionscope.sensor");
    if (!plugin) {
        std::fprintf(stderr, "FAIL: could not create the sensor\n");
        return 1;
    }
    check(plugin->init(plugin), "init");
    check(plugin->activate(plugin, RATE, 1, 4096), "activate");
    check(plugin->start_processing(plugin), "start processing");

    // Playing at 120 BPM in 3/4, from quarter note 9 (the start of bar 4), with the bar line given.
    const double qn_per_block = FRAMES / RATE * 2.0;
    clap_event_transport_t transport{};
    transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                      CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
                      CLAP_TRANSPORT_IS_PLAYING;
    transport.tempo = 120;
    transport.tsig_num = 3;
    transport.tsig_denom = 4;
    transport.bar_start = 9 * CLAP_BEATTIME_FACTOR;
    for (int b = 0; b < BLOCKS; ++b) {
        const double qn = 9 + b * qn_per_block;
        transport.song_pos_beats = static_cast<clap_beattime>(std::llround(qn * CLAP_BEATTIME_FACTOR));
        transport.song_pos_seconds = static_cast<clap_sectime>(std::llround(qn / 2.0 * CLAP_SECTIME_FACTOR));
        const std::vector<float> out = play_block(plugin, &transport, FRAMES, 0.25f);
        check(out[0] == 0.25f && out[FRAMES] == -0.25f, "audio passes through unchanged");
    }

    check(ring[0] == MAGIC && ring[1] == LAYOUT, "the sensor set up the ring header");
    check(ring[2] == BLOCKS, "one record per block");
    check(ring[10] == process_id(), "the header names this process as the host");

    const volatile double *last = ring + HDR + (BLOCKS - 1) * REC;
    check(last[1] == FRAMES && last[2] == RATE, "block size and sample rate");
    check(last[3] == 1, "playing");
    check(std::fabs(last[4] - (9 + (BLOCKS - 1) * qn_per_block) / 2.0) < 1e-6, "play position in seconds");
    check(std::fabs(last[5] - (9 + (BLOCKS - 1) * qn_per_block)) < 1e-6, "position in quarter notes");
    check(last[6] == 120 && last[7] == 3 && last[8] == 4, "tempo and time signature");
    check(last[10] == BLOCKS - 1, "sequence number");
    check(last[12] == 9, "the host's bar start");

    // A call with no samples is not a block; a host with no timeline gives NaN, never zero.
    play_block(plugin, &transport, 0, 0.25f);
    check(ring[2] == BLOCKS, "a zero-sample call writes nothing");
    play_block(plugin, nullptr, FRAMES, 0.25f);
    const volatile double *free_running = ring + HDR + BLOCKS * REC;
    check(ring[2] == BLOCKS + 1, "a block without a transport is still recorded");
    check(free_running[3] == 0 && std::isnan(free_running[5]) && std::isnan(free_running[6]) &&
              std::isnan(free_running[12]),
          "no transport: stopped, and position, tempo and bar start unknown");

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();

    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
