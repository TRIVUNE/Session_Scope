// A tiny CLAP host that checks the CLAP Probe end to end: it loads SessionScopeProbe.clap, finds
// the "probed twin" of the SessionScope Sensor, plays audio blocks through it, renames the track,
// and reads what the Probe recorded from the probes' shared table.
//
//     test_clap_probe build/native/SessionScopeProbe.clap
//
// The Probe finds plug-ins to twin in the folders named by SESSIONSCOPE_PROBE_CLAP_PATH; the
// build's test sets that to build/native, so it sees exactly one: our own Sensor.
// clap-validator checks the twin follows the CLAP rules; this checks what the Probe measures.
// Uses the real shared memory, so do not run it while a DAW has the plug-ins loaded.
// Prints "ok" and returns 0 if every check passes.

#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../sensor_core/probe_table.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
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
    return reinterpret_cast<const clap_plugin_entry_t *>(reinterpret_cast<void *>(GetProcAddress(module, "clap_entry")));
#else
    void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!module) return nullptr;
    return static_cast<const clap_plugin_entry_t *>(dlsym(module, "clap_entry"));
#endif
}

// The track name our pretend DAW reports; the test changes it to check that a rename arrives.
std::string g_track_name = "Bass";

// The host's "track-info" extension: tells a plug-in which track it is on.
const clap_host_track_info_t TRACK_INFO = {
    [](const clap_host_t *, clap_track_info_t *info) {
        info->flags = CLAP_TRACK_INFO_HAS_TRACK_NAME;
        std::strncpy(info->name, g_track_name.c_str(), CLAP_NAME_SIZE - 1);
        info->name[CLAP_NAME_SIZE - 1] = '\0';
        return true;
    },
};

// The host side: only track-info; requests from the plug-in are ignored.
const clap_host_t HOST = {
    CLAP_VERSION_INIT, nullptr, "SessionScope test host", "TRIVUNE", "", "1.0",
    [](const clap_host_t *, const char *id) -> const void * {
        return std::strcmp(id, CLAP_EXT_TRACK_INFO) == 0 ? &TRACK_INFO : nullptr;
    },
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
    [](const clap_host_t *) {},
};

// Empty event lists: no notes or parameter changes.
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

// Plays one block of `frames` samples and returns the left output, to check audio passed through.
std::vector<float> play_block(const clap_plugin_t *plugin, uint32_t frames, float value) {
    std::vector<float> in_left(FRAMES, value), in_right(FRAMES, -value);
    std::vector<float> out_left(FRAMES, 0), out_right(FRAMES, 0);
    float *in_channels[2] = {in_left.data(), in_right.data()};
    float *out_channels[2] = {out_left.data(), out_right.data()};
    clap_audio_buffer_t input{in_channels, nullptr, 2, 0, 0};
    clap_audio_buffer_t output{out_channels, nullptr, 2, 0, 0};
    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = frames;
    process.audio_inputs = &input;
    process.audio_outputs = &output;
    process.audio_inputs_count = 1;
    process.audio_outputs_count = 1;
    process.in_events = &NO_INPUT_EVENTS;
    process.out_events = &NO_OUTPUT_EVENTS;
    check(plugin->process(plugin, &process) == CLAP_PROCESS_CONTINUE, "process asks to keep running");
    return out_left;
}

// This process's slot in the probes' table, or nullptr. The test makes exactly one probe.
const probes::Slot *our_slot(const probes::Table *table) {
#ifdef _WIN32
    const std::uint64_t process_id = GetCurrentProcessId();
#else
    const std::uint64_t process_id = static_cast<std::uint64_t>(getpid());
#endif
    for (const probes::Slot &slot : table->slot) {
        if (slot.owner.load() != 0 && slot.process_id == process_id) return &slot;
    }
    return nullptr;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_clap_probe <SessionScopeProbe.clap>\n");
        return 2;
    }
    const clap_plugin_entry_t *entry = load_entry(argv[1]);
    if (!entry || !entry->init(argv[1])) {
        std::fprintf(stderr, "FAIL: could not load %s\n", argv[1]);
        return 1;
    }
    const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        std::fprintf(stderr, "FAIL: no plug-in factory\n");
        return 1;
    }

    // The Probe must list a twin of the Sensor, renamed, and never a twin of itself.
    const char *twin_id = "com.trivune.sessionscope.probe.com.trivune.sessionscope.sensor";
    const clap_plugin_descriptor_t *descriptor = nullptr;
    for (uint32_t i = 0; i < factory->get_plugin_count(factory); ++i) {
        const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, i);
        if (std::strcmp(d->id, twin_id) == 0) descriptor = d;
        check(std::strstr(d->id, "probe.com.trivune.sessionscope.probe") == nullptr, "no twin of a twin");
    }
    if (!descriptor) {
        std::fprintf(stderr, "FAIL: the Sensor's twin is not listed (is SESSIONSCOPE_PROBE_CLAP_PATH set to build/native?)\n");
        return 1;
    }
    check(std::strcmp(descriptor->name, "SessionScope Sensor (SessionScope Probe)") == 0, "the twin's name says what it is");
    check(factory->create_plugin(factory, &HOST, "com.trivune.sessionscope.probe.no.such.plugin") == nullptr, "an unknown id creates nothing");

    const clap_plugin_t *plugin = factory->create_plugin(factory, &HOST, twin_id);
    if (!plugin || !plugin->init(plugin)) {
        std::fprintf(stderr, "FAIL: could not create the Sensor's twin\n");
        return 1;
    }
    check(plugin->desc == descriptor, "the twin describes itself as the twin");
    // The real plug-in's extensions come straight through: the Sensor has audio ports.
    const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    check(ports && ports->count(plugin, true) == 1, "the Sensor's audio ports are reachable through the twin");

    check(plugin->activate(plugin, RATE, 1, FRAMES), "activate");
    check(plugin->start_processing(plugin), "start processing");
    bool passed_through = true;
    for (int i = 0; i < BLOCKS; ++i) {
        const float value = 0.01f * static_cast<float>(i + 1);
        passed_through = passed_through && play_block(plugin, FRAMES, value)[0] == value;
    }
    check(passed_through, "audio passes through unchanged");
    play_block(plugin, 0, 0);   // a call with no samples must not count as a block

    // What the Probe recorded: the real plug-in's name, our track, ten blocks of 256 samples at
    // 48 kHz. Each lasts 256 / 48000 s = 5333333 ns.
    const probes::Table *table = probes::open_table();
    const probes::Slot *slot = table ? our_slot(table) : nullptr;
    check(slot != nullptr, "the Probe took a slot in the shared table");
    if (slot) {
        check(std::string(slot->plugin) == "SessionScope Sensor", "the slot names the real plug-in");
        check(std::string(slot->track) == "Bass", "the slot names the track the host reported");
        check(slot->blocks.load() == BLOCKS, "ten blocks were timed");
        check(slot->budget_ns.load() == static_cast<std::uint64_t>(BLOCKS) * 5333333, "the blocks' length adds up");
        check(slot->cost_ns.load() > 0 && slot->cost_ns.load() < slot->budget_ns.load(), "the cost is more than nothing and less than the blocks' length");

        // The DAW renames the track and tells the plug-in: the slot follows.
        g_track_name = "Bass DI";
        const auto *track_info = static_cast<const clap_plugin_track_info_t *>(plugin->get_extension(plugin, CLAP_EXT_TRACK_INFO));
        check(track_info != nullptr, "the twin listens for track changes");
        if (track_info) track_info->changed(plugin);
        check(std::string(slot->track) == "Bass DI", "a renamed track reaches the table");
    }

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    check(table && our_slot(table) == nullptr, "destroying the twin frees its slot");
    entry->deinit();

    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
