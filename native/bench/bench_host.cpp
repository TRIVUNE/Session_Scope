// =================================================================================================
// bench_host.cpp - the offline bench: load ONE plug-in outside any DAW, play sound through it, time it
// =================================================================================================
//
// Decision 0006 (docs/decisions/0006-offline-session-analyzer.md): SessionScope measures plug-ins in
// its own host instead of inside the person's DAW. This program is that host, for one plug-in.
// `sessionscope bench` (engine/src/bench_runner.*) starts it as a separate process with a watchdog,
// so a plug-in that crashes, hangs or opens a licence window only ever takes this program down.
//
//   sessionscope_bench_host <plugin.vst3> --out <result.json> [--class NAME] [--preset FILE.vstpreset]
//                           [--rate 48000] [--block 256] [--seconds 10]
//
// What it does, in order (each step is announced on stderr as "stage: ...", so when the program
// dies the runner can say where):
//   1. load      open the .vst3 and create the plug-in (hosted_vst3.h, the Probe's hosting core)
//   2. preset    give it a saved state (.vstpreset), if one was passed: the person's own settings
//   3. setup     stereo in and out, the sample rate and block size, switched on
//   4. warm-up   one second of blocks, not timed (many plug-ins set themselves up on first use)
//   5. measure   `--seconds` of blocks, each one timed. The input is the same every run: noise
//                with a click every half second (effects), or a four-note chord on every beat
//                at 120 BPM (instruments)
//   6. check     did the plug-in really process? (the "engagement check", see below)
//   7. result    one JSON object written to --out
//
// The engagement check: a protected plug-in can load outside its DAW and then quietly do nothing
// (docs/research/offline-analyzer-scouting.md, the Ozone case). Timing such a plug-in gives a
// number that looks normal and is wrong, so every run checks the sound:
//   - an effect must change its input (allowing for its latency); one that passes the sound
//     through unchanged gets a second chance with some of its parameters moved ("nudged");
//   - an effect that outputs silence, and an instrument that stays silent while notes play, are
//     reported as such;
//   - numbers that aren't numbers (NaN, infinity) in the output are reported too.
// The runner shows the times only for "engaged" plug-ins.
//
// Timing like a DAW: blocks run on their own thread at high priority, with denormal numbers
// flushed to zero (every DAW does that on its audio thread), and the plug-in is told it runs in
// real time (kRealtime: some plug-ins use cheaper settings in real time than when rendering).
// One difference stays: the blocks run back to back, as fast as the plug-in allows, not paced by
// a sound card, so caches stay warmer than in a DAW. The calibration step (decision 0006, point 7)
// measures how much that matters.
#include "../vst3_probe/hosted_vst3.h"

#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/vstpresetfile.h"

#include <windows.h>
#include <shellapi.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace fs = std::filesystem;

namespace {

// ---- small helpers ----------------------------------------------------------------------------

void stage(const char* name) {   // "where are we": the runner reads the last one after a crash
    std::fprintf(stderr, "stage: %s\n", name);
    std::fflush(stderr);
}

std::string utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// A string as JSON text, with quotes and the characters JSON doesn't allow raw escaped.
std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char code[8];
            std::snprintf(code, sizeof code, "\\u%04x", static_cast<unsigned>(c));
            out += code;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}

// The result file: a list of "key": value lines, written as one JSON object at the end.
struct Result {
    std::vector<std::pair<std::string, std::string>> fields;   // key, value already as JSON text
    void text(const std::string& key, const std::string& value) { fields.emplace_back(key, json_string(value)); }
    void number(const std::string& key, double value) { fields.emplace_back(key, json_number(value)); }
    void raw(const std::string& key, const std::string& json) { fields.emplace_back(key, json); }

    bool write(const fs::path& path) const {
        std::ofstream file(path, std::ios::binary);
        file << "{\n";
        for (std::size_t i = 0; i < fields.size(); ++i)
            file << "  " << json_string(fields[i].first) << ": " << fields[i].second << (i + 1 < fields.size() ? ",\n" : "\n");
        file << "}\n";
        return static_cast<bool>(file);
    }
};

// A repeatable random number generator (the same noise every run, so runs can be compared).
struct Noise {
    std::uint32_t state = 0x12345678u;
    float next() {   // -1 .. 1
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
};

// The sound of the plug-in's first output channel, and of the input that went in, kept for the
// engagement check after the timed run.
struct Recording {
    std::vector<float> input, output;
    bool non_finite = false;   // a NaN or an infinity appeared in any output channel
};

// What the engagement check decided.
struct Engagement {
    std::string verdict;   // engaged, pass-through, silent, non-finite
    double change_db = 0;  // how far the output is from the (delayed) input, relative to the input
    double level_db = 0;   // the output's level, dB relative to full scale
};

double to_db(double ratio) { return ratio > 0 ? 10.0 * std::log10(ratio) : -300.0; }

Engagement check_sound(const Recording& r, std::int64_t latency, bool instrument) {
    Engagement e;
    if (r.non_finite) {
        e.verdict = "non-finite";
        return e;
    }
    const std::size_t n = std::min(r.input.size(), r.output.size());
    double in_energy = 0, out_energy = 0, diff_energy = 0;
    const auto shift = static_cast<std::size_t>(std::max<std::int64_t>(0, latency));
    for (std::size_t i = 0; i < n; ++i) {
        out_energy += double(r.output[i]) * r.output[i];
        in_energy += double(r.input[i]) * r.input[i];
        // The output at time i belongs to the input at i - latency: a plug-in that only delays
        // the sound by its reported latency counts as "unchanged".
        const double before = i >= shift ? r.input[i - shift] : 0.0;
        const double d = r.output[i] - before;
        diff_energy += d * d;
    }
    e.level_db = n ? to_db(out_energy / double(n)) : -300.0;
    if (instrument) {
        // Notes were playing for the whole run: an instrument quieter than -90 dBFS made no sound.
        e.verdict = e.level_db < -90.0 ? "silent" : "engaged";
        return e;
    }
    e.change_db = in_energy > 0 ? to_db(diff_energy / in_energy) : -300.0;
    if (out_energy < in_energy * 1e-9) e.verdict = "silent";              // 90 dB below what went in
    else if (diff_energy < in_energy * 1e-6) e.verdict = "pass-through";  // closer than -60 dB to the input
    else e.verdict = "engaged";
    return e;
}

// ---- the bench --------------------------------------------------------------------------------

struct Options {
    std::string plugin, class_name, preset;
    std::string class_id;                       // 32 hex digits: choose the plug-in by its id (a project names it so)
    std::string state_component, state_controller;   // files with a saved state's two halves (from a project)
    bool report_parameters = false;             // put every parameter's value into the result
    int program = -1;   // a factory program to choose, -1: none
    fs::path out;
    double rate = 48000;
    int block = 256;
    double seconds = 10;
};

class Bench {
public:
    Bench(const Options& o) : options_(o), effect_(("SessionScope_bench_" + std::to_string(GetCurrentProcessId())).c_str()) {}

    // Steps 1-3. False, with the reason in `error`, if the plug-in can't be used.
    bool prepare(std::string& error) {
        stage("load");
        const auto load_start = sessionscope::probes::now_ns();
        if (!options_.class_id.empty()) {
            TUID id{};
            if (!hex_id(options_.class_id, id)) {
                error = "--class-id must be 32 hex digits.";
                return false;
            }
            if (!effect_.load(options_.plugin, VST3::UID::fromTUID(id), error)) return false;
        } else if (!effect_.load_by_name(options_.plugin, options_.class_name, error)) {
            return false;
        }
        load_seconds_ = double(sessionscope::probes::now_ns() - load_start) / 1e9;
        component_ = effect_.component();
        processor_ = effect_.processor();
        controller_ = effect_.controller();
        instrument_ = effect_.sub_categories().find("Instrument") != std::string::npos;

        if (!options_.preset.empty()) {
            stage("preset");
            if (!load_preset(error)) return false;
        }
        if (!options_.state_component.empty()) {
            stage("state");
            if (!load_state(error)) return false;
        }

        find_programs();
        if (options_.program >= 0) {
            stage("program");
            if (!queue_program(error)) return false;
        }

        stage("setup");
        if (processor_->canProcessSampleSize(kSample32) != kResultOk) {
            error = "The plug-in can't process 32-bit audio.";
            return false;
        }
        use_stereo();
        ProcessSetup setup{kRealtime, kSample32, options_.block, options_.rate};
        if (effect_.setup(setup) != kResultOk) {
            error = "The plug-in refused the sample rate or block size.";
            return false;
        }
        if (!data_.prepare(*component_, options_.block, kSample32)) {
            error = "Could not make audio buffers for the plug-in's buses.";
            return false;
        }
        if (data_.numOutputs < 1 || data_.outputs[0].numChannels < 1) {
            error = "The plug-in has no audio output.";
            return false;
        }
        if (component_->setActive(true) != kResultOk) {
            error = "The plug-in would not switch on (setActive).";
            return false;
        }
        active_ = true;
        latency_ = processor_->getLatencySamples();
        return true;
    }

    // Steps 4-6, on an audio thread of our own while this (main) thread keeps Windows messages
    // moving: some plug-ins talk to hidden windows of their own on the main thread.
    void run() {
        const auto blocks_for = [&](double seconds) {
            return static_cast<std::int64_t>(std::ceil(seconds * options_.rate / options_.block));
        };
        const std::int64_t warmup = blocks_for(1.0), measured = blocks_for(options_.seconds);
        times_ns_.reserve(static_cast<std::size_t>(measured));
        recording_.input.reserve(static_cast<std::size_t>(measured * options_.block));
        recording_.output.reserve(static_cast<std::size_t>(measured * options_.block));

        std::atomic<bool> done{false};
        std::thread audio([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            _mm_setcsr(_mm_getcsr() | 0x8040);   // flush-to-zero and denormals-are-zero, as DAWs do
            processor_->setProcessing(true);

            stage("warm-up");
            collect_reported_ = true;
            for (std::int64_t i = 0; i < warmup; ++i) process_block(false, false);
            collect_reported_ = false;
            stage("measure");
            for (std::int64_t i = 0; i < measured; ++i) process_block(true, true);
            engagement_ = check_sound(recording_, latency_, instrument_);

            // The parameters as the plug-in has them now, at the settings that were timed (some
            // plug-ins finish applying a saved state only once running: the UADx LA-2A). Read by
            // the main thread, as a DAW would; this thread waits for it, before any nudge.
            snapshot_wanted_ = true;
            while (snapshot_wanted_) Sleep(1);

            // Second chance for a plug-in that left the sound alone: move some of its parameters
            // and listen again (two seconds). The times above are kept: they are the plug-in at
            // the settings it was given.
            if (engagement_.verdict == "pass-through" || engagement_.verdict == "silent") {
                stage("nudge");
                nudged_ = queue_nudge();
                recording_ = Recording{};
                for (std::int64_t i = 0; i < blocks_for(2.0); ++i) process_block(false, true);
                after_nudge_ = check_sound(recording_, latency_, instrument_);
            }
            processor_->setProcessing(false);
            done = true;
        });
        while (!done) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (snapshot_wanted_) {
                // What a DAW does with the values the audio part reported during the warm-up:
                // give them to the controller, so the list is what a DAW would show.
                if (controller_) {
                    for (const auto& [id, value] : reported_values_) controller_->setParamNormalized(id, value);
                    if (options_.report_parameters) parameters_after_run_ = parameters_now();
                }
                snapshot_wanted_ = false;
            }
            Sleep(5);
        }
        audio.join();
    }

    void report(Result& r) const {
        r.text("status", "measured");
        r.text("name", effect_.name());
        r.text("vendor", effect_.vendor());
        r.text("version", effect_.version());
        r.text("categories", effect_.sub_categories());
        r.text("kind", instrument_ ? "instrument" : "effect");
        r.raw("preset", options_.preset.empty() ? "null" : json_string(options_.preset));
        r.number("rate", options_.rate);
        r.number("block", options_.block);
        r.number("latency", double(latency_));
        r.number("load_seconds", load_seconds_);
        r.number("programs", program_count_);   // factory presets a host can choose (0: none offered)
        if (options_.program >= 0)
            r.text("program", std::size_t(options_.program) < program_names_.size() ? program_names_[std::size_t(options_.program)]
                                                                                  : std::to_string(options_.program));

        // The times, as microseconds and as a share of the block's length in real time (the
        // plug-in's share of the audio thread: 100 % means this plug-in alone fills the deadline).
        std::vector<std::uint64_t> sorted = times_ns_;
        std::sort(sorted.begin(), sorted.end());
        const double block_ns = options_.block * 1e9 / options_.rate;
        const auto at = [&](double q) {   // nearest-rank quantile
            if (sorted.empty()) return 0.0;
            const auto i = static_cast<std::size_t>(std::ceil(q * double(sorted.size()))) - (q > 0 ? 1 : 0);
            return double(sorted[std::min(i, sorted.size() - 1)]);
        };
        double sum = 0;
        for (const auto t : sorted) sum += double(t);
        const double mean = sorted.empty() ? 0 : sum / double(sorted.size());
        r.number("blocks", double(sorted.size()));
        std::ostringstream times;
        times << "{\"mean\": " << json_number(mean / 1e3) << ", \"median\": " << json_number(at(0.5) / 1e3)
              << ", \"p95\": " << json_number(at(0.95) / 1e3) << ", \"p99\": " << json_number(at(0.99) / 1e3)
              << ", \"max\": " << json_number(at(1.0) / 1e3) << "}";
        r.raw("time_us", times.str());
        std::ostringstream share;
        share << "{\"mean\": " << json_number(mean / block_ns) << ", \"p99\": " << json_number(at(0.99) / block_ns)
              << ", \"max\": " << json_number(at(1.0) / block_ns) << "}";
        r.raw("share_of_block", share.str());

        // The engagement check: the final word, and what each listen found.
        std::string verdict = engagement_.verdict;
        // After a nudge, any change counts: an effect that went from passing the sound through to
        // silence (or the other way) did respond to its parameters, so it is really running.
        if (!after_nudge_.verdict.empty())
            verdict = after_nudge_.verdict == "engaged" || after_nudge_.verdict != engagement_.verdict ? "engaged-after-nudge"
                                                                                                        : after_nudge_.verdict;
        r.text("engagement", verdict);
        std::fprintf(stderr, "engagement: %s\n", verdict.c_str());   // the build's own check looks for this line
        r.number("change_db", engagement_.change_db);
        r.number("level_db", engagement_.level_db);
        r.number("nudged_parameters", nudged_);
        if (options_.report_parameters) {
            r.raw("parameters", parameters_json_);                  // right after the state was loaded
            r.raw("parameters_after_run", parameters_after_run_);   // after the timed run, before any nudge
        }
    }

    ~Bench() {
        if (active_) component_->setActive(false);
        data_.unprepare();
    }

private:
    // "5653544B436C336B..." (32 hex digits, as REAPER writes it) to an id. REAPER writes the id as
    // the four 32-bit numbers a plug-in declares it with (FUID l1 l2 l3 l4); on Windows their
    // bytes are stored in a different order, so FUID does the conversion.
    static bool hex_id(const std::string& hex, TUID id) {
        if (hex.size() != 32) return false;
        uint32 part[4] = {};
        for (std::size_t i = 0; i < 4; ++i) {
            unsigned long value = 0;
            if (std::sscanf(hex.substr(8 * i, 8).c_str(), "%8lx", &value) != 1) return false;
            part[i] = static_cast<uint32>(value);
        }
        FUID(part[0], part[1], part[2], part[3]).toTUID(id);
        return true;
    }

    static void pump_messages(int milliseconds) {
        const auto until = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
        while (GetTickCount64() < until) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(5);
        }
    }

    static bool read_bytes(const std::string& path, std::vector<char>& bytes) {
        std::ifstream file(fs::u8path(path), std::ios::binary);
        if (!file) return false;
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    // A state saved by a DAW, given back the way a DAW does when it reopens the project: the
    // audio part's state to the audio part, the same bytes to the controller (so its knobs show
    // the same values), then the controller's own state.
    bool load_state(std::string& error) {
        std::vector<char> component_bytes, controller_bytes;
        if (!read_bytes(options_.state_component, component_bytes) ||
            (!options_.state_controller.empty() && !read_bytes(options_.state_controller, controller_bytes))) {
            error = "Can't open the state files.";
            return false;
        }
        IPtr<MemoryStream> stream = owned(new MemoryStream(component_bytes.data(), static_cast<TSize>(component_bytes.size())));
        if (component_->setState(stream) != kResultOk) {
            error = "The plug-in refused its saved state (setState).";
            return false;
        }
        if (controller_) {
            IPtr<MemoryStream> again = owned(new MemoryStream(component_bytes.data(), static_cast<TSize>(component_bytes.size())));
            controller_->setComponentState(again);
            if (!controller_bytes.empty()) {
                IPtr<MemoryStream> own = owned(new MemoryStream(controller_bytes.data(), static_cast<TSize>(controller_bytes.size())));
                controller_->setState(own);
            }
        }
        // Some plug-ins finish taking a state on the main thread a moment later (through Windows
        // messages), the way a DAW's main thread would let them: give them half a second first.
        pump_messages(500);
        if (options_.report_parameters) parameters_json_ = parameters_now();
        return true;
    }

    // Every parameter as [title, normalized value], read right after the state was loaded: the
    // proof that the state arrived (they can be compared with what the DAW showed).
    std::string parameters_now() const {
        std::string out = "[";
        const int32 count = controller_ ? controller_->getParameterCount() : 0;
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk) continue;
            const std::u16string title(reinterpret_cast<const char16_t*>(info.title));
            const std::string name = utf8(std::wstring(title.begin(), title.end()));
            out += (out.size() > 1 ? ", [" : "[") + json_string(name) + ", " +
                   json_number(controller_->getParamNormalized(info.id)) + "]";
        }
        return out + "]";
    }

    bool load_preset(std::string& error) {
        std::ifstream file(fs::u8path(options_.preset), std::ios::binary);
        if (!file) {
            error = "Can't open the preset file.";
            return false;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        IPtr<MemoryStream> stream = owned(new MemoryStream(bytes.data(), static_cast<TSize>(bytes.size())));
        const FUID id = FUID::fromTUID(effect_.class_id().data());
        // Gives the saved state to the audio part and to the controller, as a DAW does when it
        // opens a project. Fails if the preset was saved by a different plug-in.
        if (!PresetFile::loadPreset(stream, id, component_, controller_)) {
            error = "The preset could not be loaded (made by another plug-in, or not a .vstpreset).";
            return false;
        }
        return true;
    }

    // A plug-in's factory presets, the standard VST3 way: a "program list" whose names any host
    // can read (IUnitInfo), chosen through the parameter marked as the program change. Vendors'
    // own preset files (.ffp, .SerumPreset, .vpreset...) can only be opened by the plug-in's own
    // window, so this is the only way an outside host can pick one of them.
    void find_programs() {
        if (!controller_) return;
        const int32 count = controller_->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk || !(info.flags & ParameterInfo::kIsProgramChange)) continue;
            program_param_ = info.id;
            program_count_ = info.stepCount + 1;   // a program change with n steps chooses among n + 1 programs
            // The names, if the plug-in describes the list that belongs to this parameter's unit.
            // The list is the one of the parameter's unit; plug-ins that don't link the two (JUCE
            // ones) have a single list, so that one is taken.
            FUnknownPtr<IUnitInfo> units(controller_);
            if (!units) return;
            ProgramListID list = kNoProgramListId;
            for (int32 u = 0; u < units->getUnitCount(); ++u) {
                UnitInfo unit{};
                if (units->getUnitInfo(u, unit) == kResultOk && unit.id == info.unitId) list = unit.programListId;
            }
            ProgramListInfo first{};
            if (list == kNoProgramListId && units->getProgramListCount() > 0 && units->getProgramListInfo(0, first) == kResultOk)
                list = first.id;
            if (list == kNoProgramListId) return;
            for (int32 p = 0; p < program_count_; ++p) {
                String128 name{};
                units->getProgramName(list, p, name);
                program_names_.push_back(utf8(std::wstring(name, name + std::char_traits<char16>::length(name))));
            }
            return;
        }
    }

    bool queue_program(std::string& error) {
        if (program_count_ < 1) {
            error = "The plug-in offers no program list (its presets are only in its own window).";
            return false;
        }
        if (options_.program >= program_count_) {
            error = "There are only " + std::to_string(program_count_) + " programs.";
            return false;
        }
        const ParamValue value = program_count_ > 1 ? double(options_.program) / double(program_count_ - 1) : 0.0;
        controller_->setParamNormalized(program_param_, value);
        int32 index = 0, point = 0;
        if (auto* queue = nudges_.addParameterData(program_param_, index)) queue->addPoint(0, value, point);
        pending_nudge_ = true;   // delivered with the first warm-up block
        return true;
    }

    // Stereo main input (effects) and stereo main output; every other bus (sidechains, extra
    // outputs) switched off. A plug-in that refuses stereo keeps its own layout.
    void use_stereo() {
        const int32 inputs = component_->getBusCount(kAudio, kInput), outputs = component_->getBusCount(kAudio, kOutput);
        std::vector<SpeakerArrangement> in(static_cast<std::size_t>(std::max<int32>(inputs, 0)));
        std::vector<SpeakerArrangement> out(static_cast<std::size_t>(std::max<int32>(outputs, 0)));
        for (int32 i = 0; i < inputs; ++i) processor_->getBusArrangement(kInput, i, in[static_cast<std::size_t>(i)]);
        for (int32 i = 0; i < outputs; ++i) processor_->getBusArrangement(kOutput, i, out[static_cast<std::size_t>(i)]);
        if (!in.empty()) in[0] = SpeakerArr::kStereo;
        if (!out.empty()) out[0] = SpeakerArr::kStereo;
        processor_->setBusArrangements(in.data(), inputs, out.data(), outputs);
        for (int32 i = 0; i < inputs; ++i) component_->activateBus(kAudio, kInput, i, i == 0);
        for (int32 i = 0; i < outputs; ++i) component_->activateBus(kAudio, kOutput, i, i == 0);
        if (component_->getBusCount(kEvent, kInput) > 0) component_->activateBus(kEvent, kInput, 0, true);
    }

    // Moves the plug-in's automatable parameters (up to 512) halfway around their range: with
    // that many changes, a plug-in that really runs almost surely sounds different. (Moving only
    // the first 16 was too weak: Pro-Q 4 and Ozone 12 stayed bit-exact.) The changes reach the
    // audio part with the next block, and the controller is told too.
    int queue_nudge() {
        if (!controller_) return 0;
        int moved = 0;
        const int32 count = controller_->getParameterCount();
        for (int32 i = 0; i < count && moved < 512; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk) continue;
            const bool skip = (info.flags & (ParameterInfo::kIsBypass | ParameterInfo::kIsReadOnly |
                                             ParameterInfo::kIsProgramChange)) != 0 ||
                              (info.flags & ParameterInfo::kCanAutomate) == 0;
            if (skip) continue;
            const ParamValue now = controller_->getParamNormalized(info.id);
            const ParamValue moved_to = now < 0.5 ? now + 0.5 : now - 0.5;
            controller_->setParamNormalized(info.id, moved_to);
            int32 index = 0;
            if (auto* queue = nudges_.addParameterData(info.id, index)) {
                int32 point = 0;
                queue->addPoint(0, moved_to, point);
            }
            ++moved;
        }
        pending_nudge_ = moved > 0;
        return moved;
    }

    // One audio block: fill the input (or the notes), call process(), keep what came out.
    void process_block(bool timed, bool keep_sound) {
        const int32 n = options_.block;
        const double block_start = double(position_);

        // Effects: the test signal into every channel of the main input (each channel gets its own
        // noise, so a stereo effect sees a real stereo signal). Every other input (a sidechain,
        // or an instrument's audio input) gets silence.
        for (int32 b = 0; b < data_.numInputs; ++b)
            for (int32 c = 0; c < data_.inputs[b].numChannels; ++c)
                std::fill_n(data_.inputs[b].channelBuffers32[c], n, 0.0f);
        if (data_.numInputs > 0 && !instrument_) {
            AudioBusBuffers& bus = data_.inputs[0];
            for (int32 s = 0; s < n; ++s) {
                const std::int64_t t = position_ + s;
                const bool click = (t % std::int64_t(options_.rate / 2)) < 64;   // a short burst every half second
                for (int32 c = 0; c < bus.numChannels; ++c)
                    bus.channelBuffers32[c][s] = click ? 0.9f : 0.25f * noise_.next();
            }
        }

        // Instruments: a chord on every beat at 120 BPM (a beat is half a second), held 0.4 s.
        events_.clear();
        if (instrument_) {
            const std::int64_t beat = std::int64_t(options_.rate / 2), hold = std::int64_t(options_.rate * 0.4);
            static constexpr int16 chord[] = {48, 60, 64, 67};   // C3, C4, E4, G4
            for (int32 s = 0; s < n; ++s) {
                const std::int64_t t = position_ + s, in_beat = t % beat;
                if (in_beat != 0 && in_beat != hold) continue;
                for (const int16 pitch : chord) {
                    Event e{};
                    e.busIndex = 0;
                    e.sampleOffset = s;
                    e.ppqPosition = double(t) / beat;
                    if (in_beat == 0) {
                        e.type = Event::kNoteOnEvent;
                        e.noteOn = {0, pitch, 0.0f, 0.8f, 0, -1};
                    } else {
                        e.type = Event::kNoteOffEvent;
                        e.noteOff = {0, pitch, 0.0f, -1, 0.0f};
                    }
                    events_.addEvent(e);
                }
            }
        }

        // Where the song is: playing, 120 BPM, 4/4, from the start.
        context_ = ProcessContext{};
        context_.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid |
                         ProcessContext::kProjectTimeMusicValid | ProcessContext::kBarPositionValid |
                         ProcessContext::kSystemTimeValid;
        context_.sampleRate = options_.rate;
        context_.projectTimeSamples = position_;
        context_.systemTime = static_cast<int64>(sessionscope::probes::now_ns());
        context_.tempo = 120;
        context_.timeSigNumerator = 4;
        context_.timeSigDenominator = 4;
        context_.projectTimeMusic = block_start / (options_.rate / 2);
        context_.barPositionMusic = std::floor(context_.projectTimeMusic / 4) * 4;

        data_.numSamples = n;
        data_.processContext = &context_;
        data_.inputEvents = &events_;
        data_.inputParameterChanges = pending_nudge_ ? &nudges_ : nullptr;
        // Where the audio part reports values it changed itself (a DAW passes them on to the
        // controller, so its knobs follow).
        reported_.clearQueue();
        data_.outputParameterChanges = &reported_;

        // The input is kept before the call: a careless plug-in may write into its input buffers.
        if (keep_sound) {
            const bool has_input = data_.numInputs > 0 && data_.inputs[0].numChannels > 0;
            for (int32 s = 0; s < n; ++s) recording_.input.push_back(has_input ? data_.inputs[0].channelBuffers32[0][s] : 0.0f);
        }

        const auto start = sessionscope::probes::now_ns();
        processor_->process(data_);
        const auto elapsed = sessionscope::probes::now_ns() - start;
        if (timed) times_ns_.push_back(elapsed);
        // Keep the last value of each parameter the audio part reported (outside the timing).
        for (int32 q = 0; collect_reported_ && q < reported_.getParameterCount(); ++q) {
            IParamValueQueue* queue = reported_.getParameterData(q);
            ParamValue value = 0;
            int32 offset = 0;
            if (queue && queue->getPointCount() > 0 && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk)
                reported_values_.emplace_back(queue->getParameterId(), value);
        }
        if (pending_nudge_) {   // the changes are delivered once
            nudges_.clearQueue();
            pending_nudge_ = false;
        }

        // Keep the first output channel, and look for NaN or infinity in every one.
        if (keep_sound) {
            const AudioBusBuffers& out = data_.outputs[0];
            for (int32 c = 0; c < out.numChannels; ++c)
                for (int32 s = 0; s < n; ++s)
                    if (!std::isfinite(out.channelBuffers32[c][s])) recording_.non_finite = true;
            for (int32 s = 0; s < n; ++s) recording_.output.push_back(out.channelBuffers32[0][s]);
        }
        position_ += n;
    }

    Options options_;
    sessionscope::HostedVst3 effect_;
    IComponent* component_ = nullptr;
    IAudioProcessor* processor_ = nullptr;
    IEditController* controller_ = nullptr;
    bool instrument_ = false, active_ = false, pending_nudge_ = false;
    double load_seconds_ = 0;
    std::int64_t latency_ = 0, position_ = 0;
    int nudged_ = 0;
    ParamID program_param_ = 0;               // the program-change parameter, if there is one
    int32 program_count_ = 0;                 // how many programs it chooses among
    std::vector<std::string> program_names_;  // their names, when the plug-in gives them
    std::string parameters_json_ = "[]";      // the parameters right after a saved state was loaded
    std::string parameters_after_run_ = "[]"; // and again once the plug-in has been running

    HostProcessData data_;        // the audio buffers for every bus of the plug-in
    EventList events_{512};       // the notes of one block
    ParameterChanges nudges_{512}; // the nudge, delivered with one block
    ParameterChanges reported_{4096};   // values the audio part reports back, one block at a time
    std::vector<std::pair<ParamID, ParamValue>> reported_values_;   // all of them, in order, during warm-up
    bool collect_reported_ = false;     // true during the warm-up
    std::atomic<bool> snapshot_wanted_{false};   // the audio thread asks the main thread to read the parameters
    ProcessContext context_{};
    Noise noise_;
    std::vector<std::uint64_t> times_ns_;
    Recording recording_;
    Engagement engagement_, after_nudge_;
};

bool parse(int argc, wchar_t** argv, Options& o, std::string& error) {
    if (argc < 2) {
        error = "usage: sessionscope_bench_host <plugin.vst3> --out FILE [--class NAME] [--preset FILE] "
                "[--rate HZ] [--block SAMPLES] [--seconds N]";
        return false;
    }
    o.plugin = utf8(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (i + 1 >= argc) {
            error = "Missing a value after " + utf8(option);
            return false;
        }
        const std::string value = utf8(argv[++i]);
        try {
            if (option == L"--out") o.out = fs::u8path(value);
            else if (option == L"--class") o.class_name = value;
            else if (option == L"--class-id") o.class_id = value;
            else if (option == L"--state-component") o.state_component = value;
            else if (option == L"--state-controller") o.state_controller = value;
            else if (option == L"--report-parameters") o.report_parameters = value == "1";
            else if (option == L"--preset") o.preset = value;
            else if (option == L"--program") o.program = std::stoi(value);
            else if (option == L"--rate") o.rate = std::stod(value);
            else if (option == L"--block") o.block = std::stoi(value);
            else if (option == L"--seconds") o.seconds = std::stod(value);
            else {
                error = "Unknown option " + utf8(option);
                return false;
            }
        } catch (const std::exception&) {
            error = "Not a number: " + value;
            return false;
        }
    }
    if (o.out.empty()) error = "--out is required";
    else if (o.rate < 8000 || o.rate > 768000) error = "--rate must be between 8000 and 768000";
    else if (o.block < 16 || o.block > 8192) error = "--block must be between 16 and 8192";
    else if (o.seconds <= 0 || o.seconds > 600) error = "--seconds must be between 0 and 600";
    return error.empty();
}

}  // namespace

int main() {
    // A crash must end this program at once, without Windows' "stopped working" window, which
    // would keep the process alive until someone clicked it (the runner would wait for nothing).
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);   // file names may not be ASCII
    Options options;
    std::string error;
    if (!parse(argc, argv, options, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    Result result;
    result.text("plugin", options.plugin);
    {
        Bench bench(options);
        if (!bench.prepare(error)) {
            result.text("status", "failed");
            result.text("error", error);
            result.write(options.out);
            return 3;
        }
        bench.run();
        stage("result");
        bench.report(result);
        // Written before the plug-in is unloaded: if it crashes on the way out, the numbers are
        // kept and the runner reports the crash next to them.
        if (!result.write(options.out)) return 4;
        stage("unload");
    }
    stage("done");
    return 0;
}
