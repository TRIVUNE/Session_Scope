// =================================================================================================
// sessionscope_clap_probe.cpp - the SessionScope Probe for CLAP: times any CLAP plug-in
// =================================================================================================
//
// The CLAP sibling of the VST3 Probe (../vst3_probe): it measures how much of each audio block a
// plug-in uses and writes that into the probes' shared table (../sensor_core/probe_table.h) for
// the HUD's Task Manager. It works differently from the VST3 one, because CLAP allows a simpler
// way:
//
//   This file shows the DAW one "probed" twin of every CLAP plug-in installed on the computer:
//   next to "Some Reverb" the DAW lists "Some Reverb (SessionScope Probe)". There is nothing to
//   pick and no window of ours. Using the twin instead of the original is the whole workflow.
//
// How a twin works. A CLAP plug-in is a small table of functions (clap_plugin_t: init, activate,
// process, get_extension...) plus one pointer, plugin_data, to the plug-in's own memory. When the
// DAW creates a twin, we create the real plug-in, copy its table, and replace a handful of
// entries with ours:
//   process         ours: read the clock, call the real process, read the clock again, record
//   activate        ours: remember the sample rate (to know how long a block lasts), then the real one
//   init, destroy   ours: take and free our slot in the shared table around the real ones
//   get_extension   ours: only to hear about the track's name; everything else is the real one's
// Every other entry, and every extension (parameters, state, window, latency, notes...), is the
// real plug-in's own function, and plugin_data is the real plug-in's own pointer. So the DAW talks
// to the real plug-in directly for all of that, and the real plug-in talks to the DAW directly
// (it is given the DAW's own host object). Presets, automation and the plug-in's window behave
// exactly as without the Probe. A project saved with a twin opens only where the Probe is installed.
//
// The limit of this trick: a plug-in must find its own memory through plugin_data, as the CLAP
// headers intend. One that instead compares the clap_plugin_t pointer itself with one it kept
// would be confused by our copy. None we tried does; if one misbehaves, use the original.
//
// Where the twins come from: the standard CLAP folders (the same ones a DAW scans), or only the
// folders in the environment variable SESSIONSCOPE_PROBE_CLAP_PATH when that is set (the tests use
// it to see only our own plug-ins). The CLAP rules are in the headers, github.com/free-audio/clap
// (entry.h, plugin.h, factory/plugin-factory.h, ext/track-info.h).
//
// Windows and Linux. macOS keeps a .clap as a bundle folder and needs its own loading code: not done.

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include "../sensor_core/probe_table.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace probes = sessionscope::probes;

namespace {

// Every twin's id starts with this, followed by the real plug-in's id. Never to change: DAWs
// remember plug-ins in saved projects by id.
constexpr const char *ID_PREFIX = "com.trivune.sessionscope.probe.";
constexpr const char *NAME_SUFFIX = " (SessionScope Probe)";

// ---- finding and loading the real plug-ins --------------------------------------------------------

// One real plug-in we can make a twin of.
struct Original {
    const clap_plugin_factory_t *factory = nullptr;   // the real file's factory: creates the real plug-in
    const clap_plugin_descriptor_t *real = nullptr;   // the real plug-in's own description
    std::string id, name;                             // the twin's id and name (the descriptor below points at these)
    clap_plugin_descriptor_t twin{};                  // what we show the DAW: the real description, renamed
};

// Filled once, the first time the DAW asks what this file holds. unique_ptr: the DAW keeps
// pointers to the descriptors, so they must never move.
std::vector<std::unique_ptr<Original>> g_originals;
std::once_flag g_scanned;

// Text in the form the system's file functions use: wide (UTF-16) on Windows, bytes elsewhere.
using NativeText = fs::path::string_type;

// An environment variable; empty when it isn't set. Windows keeps these as wide text, so a user
// name with letters outside ASCII survives.
NativeText env(const char *name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::strlen(name));   // the names are plain ASCII
    const wchar_t *value = _wgetenv(wide_name.c_str());
    return value ? value : L"";
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}

// This file's own path: the scan must never load the Probe into itself.
fs::path own_path() {
#ifdef _WIN32
    HMODULE module = nullptr;   // the DLL this very function is in
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&own_path), &module);
    wchar_t buffer[32768];      // the longest path Windows allows
    const DWORD length = module ? GetModuleFileNameW(module, buffer, 32768) : 0;
    return fs::path(std::wstring(buffer, length));
#else
    Dl_info info{};
    return dladdr(reinterpret_cast<void *>(&own_path), &info) && info.dli_fname ? fs::path(info.dli_fname) : fs::path();
#endif
}

// The folders to look in. CLAP's standard places (entry.h lists them), or the test override.
std::vector<fs::path> search_folders() {
    std::vector<fs::path> folders;
#ifdef _WIN32
    const NativeText::value_type separator = L';';
#else
    const NativeText::value_type separator = ':';
#endif
    const auto add_list = [&](const NativeText &list) {   // "a;b;c" -> three folders
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t end = list.find(separator, start);
            const NativeText one = list.substr(start, end == NativeText::npos ? NativeText::npos : end - start);
            if (!one.empty()) folders.emplace_back(one);
            if (end == NativeText::npos) break;
            start = end + 1;
        }
    };
    const NativeText only = env("SESSIONSCOPE_PROBE_CLAP_PATH");
    if (!only.empty()) {   // the tests: look here and nowhere else
        add_list(only);
        return folders;
    }
#ifdef _WIN32
    if (!env("COMMONPROGRAMFILES").empty()) folders.push_back(fs::path(env("COMMONPROGRAMFILES")) / "CLAP");
    if (!env("LOCALAPPDATA").empty()) folders.push_back(fs::path(env("LOCALAPPDATA")) / "Programs" / "Common" / "CLAP");
#else
    folders.push_back("/usr/lib/clap");
    if (!env("HOME").empty()) folders.push_back(fs::path(env("HOME")) / ".clap");
#endif
    add_list(env("CLAP_PATH"));   // extra folders the user set, as CLAP defines
    return folders;
}

// Loads one .clap file and returns its entry point (the "clap_entry" symbol), or nullptr.
// The file stays loaded for as long as the DAW runs: its plug-ins may be in use.
const clap_plugin_entry_t *load_entry(const fs::path &file) {
#ifdef _WIN32
    const HMODULE module = LoadLibraryW(file.c_str());
    if (!module) return nullptr;
    return reinterpret_cast<const clap_plugin_entry_t *>(reinterpret_cast<void *>(GetProcAddress(module, "clap_entry")));
#else
    void *module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) return nullptr;
    return static_cast<const clap_plugin_entry_t *>(dlsym(module, "clap_entry"));
#endif
}

// Looks through every folder for .clap files and lists their plug-ins as originals.
void scan() {
    const fs::path self = own_path();
    std::vector<fs::path> seen;   // files already loaded (a folder can be listed twice)
    for (const fs::path &folder : search_folders()) {
        // `walk` collects the walk's failures instead of throwing: a folder that isn't there is
        // normal, and simply ends this folder. `ignored` is for the small checks inside.
        std::error_code walk, ignored;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, walk), end;
             !walk && it != end; it.increment(walk)) {
            const fs::path file = it->path();
            if (file.extension() != ".clap" || !it->is_regular_file(ignored)) continue;
            if (!self.empty() && fs::equivalent(file, self, ignored)) continue;             // ourselves
            const fs::path real_path = fs::weakly_canonical(file, ignored);
            if (std::find(seen.begin(), seen.end(), real_path) != seen.end()) continue;     // already done
            seen.push_back(real_path);

            const clap_plugin_entry_t *entry = load_entry(file);
            if (!entry || !clap_version_is_compatible(entry->clap_version)) continue;
            if (!entry->init(file.u8string().c_str())) continue;                            // the file says it can't start
            const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
            if (!factory) continue;

            for (uint32_t i = 0, count = factory->get_plugin_count(factory); i < count; ++i) {
                const clap_plugin_descriptor_t *real = factory->get_plugin_descriptor(factory, i);
                if (!real || !real->id || !real->name) continue;
                if (std::strncmp(real->id, ID_PREFIX, std::strlen(ID_PREFIX)) == 0) continue;   // a twin of a twin: no
                auto original = std::make_unique<Original>();
                original->factory = factory;
                original->real = real;
                original->id = std::string(ID_PREFIX) + real->id;
                original->name = std::string(real->name) + NAME_SUFFIX;
                original->twin = *real;                                 // same vendor, version, features...
                original->twin.id = original->id.c_str();               // ...but our id
                original->twin.name = original->name.c_str();           // and a name that says what it is
                g_originals.push_back(std::move(original));
            }
        }
    }
}

const std::vector<std::unique_ptr<Original>> &originals() {
    std::call_once(g_scanned, scan);   // once, however many threads ask
    return g_originals;
}

// ---- one twin --------------------------------------------------------------------------------------

// What the DAW gets when it creates a twin. `plugin` must stay the first member: the DAW hands our
// functions a pointer to it, and that pointer is then also a pointer to the whole Twin.
struct Twin {
    clap_plugin_t plugin{};                       // the real plug-in's table, with a few entries replaced
    const clap_plugin_t *real = nullptr;          // the real plug-in
    const clap_host_t *host = nullptr;            // the DAW's host object (the real plug-in has it too)
    const Original *original = nullptr;           // which plug-in this is a twin of
    probes::Writer writer;                        // our slot in the shared table
    double sample_rate = 0;                       // from activate; 0 until then

    Twin() : writer(this) {}                      // the writer takes our address as part of its id
};

Twin *twin_of(const clap_plugin_t *plugin) { return reinterpret_cast<Twin *>(const_cast<clap_plugin_t *>(plugin)); }

// Asks the DAW which track we are on and writes the names into the table. Main thread.
void publish_names(Twin *twin) {
    if (!twin->writer.active()) return;
    clap_track_info_t info{};
    const char *track = "Unknown track";          // what the HUD shows when the DAW doesn't say
    const auto *track_info = static_cast<const clap_host_track_info_t *>(twin->host->get_extension(twin->host, CLAP_EXT_TRACK_INFO));
    if (!track_info)                              // some DAWs still use the name from before CLAP 1.2
        track_info = static_cast<const clap_host_track_info_t *>(twin->host->get_extension(twin->host, CLAP_EXT_TRACK_INFO_COMPAT));
    if (track_info && track_info->get(twin->host, &info) && (info.flags & CLAP_TRACK_INFO_HAS_TRACK_NAME)) {
        info.name[CLAP_NAME_SIZE - 1] = '\0';     // always ends in a zero
        if (info.name[0]) track = info.name;
    }
#ifdef _WIN32
    const std::uint64_t process_id = GetCurrentProcessId();   // the DAW's process: the HUD shows only its own DAW's probes
#else
    const std::uint64_t process_id = static_cast<std::uint64_t>(getpid());
#endif
    // CLAP doesn't tell a plug-in its track's position, so the order is -1 (unknown): the HUD
    // then sorts these tracks by name.
    twin->writer.set_names(track, twin->original->real->name, -1, process_id);
}

// init: the real plug-in first; if it starts, take our slot in the table.
bool twin_init(const clap_plugin_t *plugin) {
    Twin *twin = twin_of(plugin);
    if (!twin->real->init(twin->real)) return false;
    if (twin->writer.claim()) publish_names(twin);   // a full table only means we don't report
    return true;
}

// destroy: the real plug-in frees itself; then we free the twin (its writer gives the slot back).
void twin_destroy(const clap_plugin_t *plugin) {
    Twin *twin = twin_of(plugin);
    twin->real->destroy(twin->real);
    delete twin;
}

// activate: the DAW tells the sample rate here. Also a good moment to ask for the track's name
// again: a DAW may not have known it yet during init.
bool twin_activate(const clap_plugin_t *plugin, double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    Twin *twin = twin_of(plugin);
    if (!twin->real->activate(twin->real, sample_rate, min_frames, max_frames)) return false;
    twin->sample_rate = sample_rate;
    publish_names(twin);
    return true;
}

// One audio block. Audio thread: only the clock and atomic adds besides the real plug-in's work.
clap_process_status twin_process(const clap_plugin_t *plugin, const clap_process_t *process) {
    Twin *twin = twin_of(plugin);
    // No samples (only events), or no sample rate yet: pass it on without timing.
    if (process->frames_count == 0 || twin->sample_rate <= 0) return twin->real->process(twin->real, process);
    const std::uint64_t start = probes::now_ns();
    const clap_process_status status = twin->real->process(twin->real, process);
    const std::uint64_t elapsed = probes::now_ns() - start;
    // How long this block lasts when played: samples / samples per second, in nanoseconds.
    const auto budget = static_cast<std::uint64_t>(process->frames_count * 1e9 / twin->sample_rate);
    twin->writer.record_block(elapsed, budget);
    return status;
}

// The "track-info" extension, plug-in side: the DAW calls changed() when the track is renamed.
// We refresh our names, then tell the real plug-in, if it listens for this too.
void twin_track_changed(const clap_plugin_t *plugin) {
    Twin *twin = twin_of(plugin);
    publish_names(twin);
    for (const char *id : {CLAP_EXT_TRACK_INFO, CLAP_EXT_TRACK_INFO_COMPAT}) {
        const auto *theirs = static_cast<const clap_plugin_track_info_t *>(twin->real->get_extension(twin->real, id));
        if (theirs && theirs->changed) {
            theirs->changed(twin->real);
            break;
        }
    }
}
const clap_plugin_track_info_t TRACK_INFO = {twin_track_changed};

// get_extension: ours for track-info, the real plug-in's own for everything else.
const void *twin_get_extension(const clap_plugin_t *plugin, const char *id) {
    if (std::strcmp(id, CLAP_EXT_TRACK_INFO) == 0 || std::strcmp(id, CLAP_EXT_TRACK_INFO_COMPAT) == 0) return &TRACK_INFO;
    Twin *twin = twin_of(plugin);
    return twin->real->get_extension(twin->real, id);
}

// ---- the factory: what the DAW asks -----------------------------------------------------------------

uint32_t plugin_count(const clap_plugin_factory_t *) { return static_cast<uint32_t>(originals().size()); }

const clap_plugin_descriptor_t *plugin_descriptor(const clap_plugin_factory_t *, uint32_t index) {
    return index < originals().size() ? &originals()[index]->twin : nullptr;
}

const clap_plugin_t *create_plugin(const clap_plugin_factory_t *, const clap_host_t *host, const char *id) {
    if (!host || !id || !clap_version_is_compatible(host->clap_version)) return nullptr;
    for (const auto &original : originals()) {
        if (original->id != id) continue;
        // The real plug-in gets the DAW's own host object, so everything it asks of the DAW goes straight there.
        const clap_plugin_t *real = original->factory->create_plugin(original->factory, host, original->real->id);
        if (!real) return nullptr;
        Twin *twin = new (std::nothrow) Twin();
        if (!twin) {
            real->destroy(real);
            return nullptr;
        }
        twin->real = real;
        twin->host = host;
        twin->original = original.get();
        twin->plugin = *real;                             // every function and plugin_data: the real plug-in's
        twin->plugin.desc = &original->twin;              // our description
        twin->plugin.init = twin_init;                    // and the few entries we need (see the top of this file)
        twin->plugin.destroy = twin_destroy;
        twin->plugin.activate = twin_activate;
        twin->plugin.process = twin_process;
        twin->plugin.get_extension = twin_get_extension;
        return &twin->plugin;
    }
    return nullptr;
}

const clap_plugin_factory_t FACTORY = {plugin_count, plugin_descriptor, create_plugin};

}  // namespace

// The one symbol a CLAP host looks for in the file.
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char *) { return true; },   // init: nothing yet; the scan waits until the DAW asks what we hold
    []() {},                             // deinit: the loaded plug-in files stay loaded until the DAW quits
    [](const char *factory_id) -> const void * {
        return std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &FACTORY : nullptr;
    },
};
