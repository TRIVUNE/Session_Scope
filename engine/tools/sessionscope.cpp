// The sessionscope command-line program: record a trace from a running sensor (in any DAW), list
// and report on recorded traces, write synthetic ones, install the plug-ins (engine/src/install.h)
// and run the REAPER tools (engine/src/reaper_tools.h). Built by CMake into build/engine/.

#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

#include "bench_runner.h"
#include "install.h"
#include "json.h"
#include "probe_monitor.h"
#include "reaper_tools.h"
#include "als_reader.h"
#include "rpp_reader.h"
#include "report.h"
#include "ring_reader.h"
#include "simulate.h"
#include "trace_folders.h"
#include "trace_recorder.h"

namespace fs = std::filesystem;
using namespace sessionscope;

namespace {

constexpr const char *USAGE =
    "Usage:\n"
    "  sessionscope record [--root DIR] [--seconds N]   record a trace from the sensor (Ctrl+C stops)\n"
    "  sessionscope list [--root DIR]                   list recorded traces, oldest first\n"
    "  sessionscope report [TRACE] [--root DIR] [--metric KEY] [--quantile Q] [--min-blocks N]\n"
    "                       [--json] [--no-color] [--ascii] [--color]\n"
    "                       report on a trace\n"
    "  sessionscope simulate OUT [--scenario NAME] [--seed N] [--drop-every N] [--reset-at N]\n"
    "                       [--truncate] [--no-host]\n"
    "                       write a synthetic trace\n"
    "  sessionscope probes [--json]                     CPU per track and plug-in, from the SessionScope Probes\n"
    "  sessionscope bench PLUGIN.vst3... | --project FILE.rpp\n"
    "                       [--class NAME] [--preset FILE] [--program N] [--rate HZ] [--block N]\n"
    "                       [--seconds N] [--timeout N] [--out DIR] [--parameters] [--json]\n"
    "                       time plug-ins offline, each in its own process (Windows); from a\n"
    "                       project, each with the settings saved in it\n"
    "  sessionscope install [--uninstall] [--from DIR]  copy the built plug-ins to where DAWs look\n"
    "  sessionscope reaper install|uninstall|validate   the REAPER scripts and the validation run\n"
    "  sessionscope --version\n"
    "  sessionscope --help\n";

std::atomic<bool> g_stop_requested{false};

void request_stop(int) { g_stop_requested = true; }

std::string display_text(const fs::path &path) { return path.u8string(); }

std::string lower_extension(const fs::path &path) {   // ".als", ".rpp"... in lower case
    std::string ext = path.extension().u8string();
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// The options both commands share. Returns false, after explaining why, if an argument is wrong.
bool parse_options(int argc, char *argv[], bool allow_seconds, std::optional<fs::path> &root, double &seconds) {
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--root" && has_value) {
            root = fs::u8path(argv[++i]);
        } else if (option == "--seconds" && has_value && allow_seconds) {
            char *end = nullptr;
            seconds = std::strtod(argv[++i], &end);
            if (*end != '\0' || !(seconds > 0)) {
                std::fprintf(stderr, "--seconds needs a positive number\n%s", USAGE);
                return false;
            }
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n%s", option.c_str(), USAGE);
            return false;
        }
    }
    return true;
}

int record_command(int argc, char *argv[]) {
    std::optional<fs::path> root;
    double seconds = 0;
    if (!parse_options(argc, argv, true, root, seconds)) return 2;

    const fs::path folder = root.value_or(default_trace_root()) / new_trace_folder_name();
    SharedSensor sensor;
    TraceRecorder recorder(folder, sensor);

    std::signal(SIGINT, request_stop);
    std::printf("Waiting for the sensor (put SessionScope Sensor on the master track and start audio)...\n");
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    const auto time_is_up = [&] {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        return seconds > 0 && elapsed.count() >= seconds;
    };

    bool announced_recording = false;
    bool warned_second_sensor = false;
    while (!g_stop_requested && !time_is_up() && !recorder.failed()) {
        recorder.poll();
        if (recorder.started() && !announced_recording) {
            std::printf("Recording to %s (Ctrl+C stops)\n", display_text(folder).c_str());
            std::fflush(stdout);
            announced_recording = true;
        }
        if (recorder.started() && !warned_second_sensor && sensor.second_sensor()) {
            std::fprintf(stderr, "Warning: a second SessionScope Sensor is running; only one is recorded.\n");
            warned_second_sensor = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    recorder.finish();

    if (recorder.failed()) {
        std::fprintf(stderr, "Could not write to %s. Is the disk full?\n", display_text(folder).c_str());
        return 1;
    }
    if (!recorder.started()) {
        std::printf("Nothing recorded: no sensor data arrived.\n");
        return 1;
    }
    std::printf("Recorded %zu blocks to %s\n", recorder.rows_written(), display_text(folder).c_str());
    return 0;
}

std::string read_whole_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A whole number from meta.json as text, or "?" when it is missing: a missing value is not a zero.
std::string count_text(const JsonValue &value) {
    return value.is_number() ? std::to_string(static_cast<long long>(value.number())) : "?";
}

void print_trace_line(const fs::path &folder) {
    // A broken or half-written meta.json parses to nothing, and the trace is still listed.
    const JsonValue meta = parse_json(read_whole_file(folder / "meta.json")).value_or(JsonValue());
    const JsonValue &summary = meta["summary"];
    std::printf("%s  %-28s %7s s  %8s blocks  %s xruns\n", display_text(folder.filename()).c_str(),
                meta["project"]["name"].text().c_str(), count_text(meta["duration_s"]).c_str(),
                count_text(summary["sensor_records"]).c_str(), count_text(summary["audio_xruns"]).c_str());
}

int list_command(int argc, char *argv[]) {
    std::optional<fs::path> root;
    double unused_seconds = 0;
    if (!parse_options(argc, argv, false, root, unused_seconds)) return 2;

    const auto folders = all_trace_folders(root);
    if (folders.empty()) {
        std::printf("No traces found.\nLooked in: %s\n",
                    root ? display_text(*root).c_str()
                         : (display_text(default_trace_root()) + " and " + display_text(reaper_trace_root())).c_str());
        return 0;
    }
    for (const fs::path &folder : folders) print_trace_line(folder);
    return 0;
}

int simulate_command(int argc, char *argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Missing output folder\n%s", USAGE);
        return 2;
    }

    fs::path out = fs::u8path(argv[2]);
    std::string scenario = "song";
    int seed = 1;
    int drop_every = 0;
    std::optional<int> reset_at;
    bool truncate = false;
    bool host = true;

    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--scenario" && has_value) {
            scenario = argv[++i];
        } else if (option == "--seed" && has_value) {
            char *end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || val < 0) {
                std::fprintf(stderr, "--seed needs a non-negative integer\n%s", USAGE);
                return 2;
            }
            seed = static_cast<int>(val);
        } else if (option == "--drop-every" && has_value) {
            char *end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || val < 0) {
                std::fprintf(stderr, "--drop-every needs a non-negative integer\n%s", USAGE);
                return 2;
            }
            drop_every = static_cast<int>(val);
        } else if (option == "--reset-at" && has_value) {
            char *end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || val <= 0) {
                std::fprintf(stderr, "--reset-at needs a positive integer\n%s", USAGE);
                return 2;
            }
            reset_at = static_cast<int>(val);
        } else if (option == "--truncate") {
            truncate = true;
        } else if (option == "--no-host") {
            host = false;
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n%s", option.c_str(), USAGE);
            return 2;
        }
    }

    const Scenario *sc = scenario_by_name(scenario);
    if (!sc) {
        std::fprintf(stderr, "Unknown scenario: %s\n%s", scenario.c_str(), USAGE);
        return 2;
    }

    SimulateOptions opts;
    opts.seed = seed;
    opts.srate = 48000;
    opts.frames = 256;
    opts.drop_every = drop_every;
    opts.reset_at = reset_at;
    opts.truncate = truncate;
    opts.host = host;

    try {
        auto folder = write_simulated_trace(out, *sc, opts);
        std::printf("Synthetic trace written to %s\n", display_text(folder).c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}

struct ReportOptions {
    std::optional<fs::path> trace;
    std::optional<fs::path> root;
    std::string metric = "lateness";
    double quantile = 0.99;
    int min_blocks = DEFAULT_MIN_BLOCKS;
    bool json = false;
    std::optional<bool> color;
    std::optional<bool> unicode;
};

bool parse_report_options(int argc, char *argv[], ReportOptions &opts) {
    static const std::vector<std::string> valid_metrics = {"lateness", "workload", "host_rt", "lateness_ms", "interval"};

    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--root" && has_value) {
            opts.root = fs::u8path(argv[++i]);
        } else if (option == "--metric" && has_value) {
            const std::string &m = argv[++i];
            if (std::find(valid_metrics.begin(), valid_metrics.end(), m) == valid_metrics.end()) {
                std::fprintf(stderr, "Unknown metric: %s. Valid: lateness, workload, host_rt, lateness_ms, interval\n%s", m.c_str(), USAGE);
                return false;
            }
            opts.metric = m;
        } else if (option == "--quantile" && has_value) {
            char *end = nullptr;
            opts.quantile = std::strtod(argv[++i], &end);
            if (*end != '\0' || !(opts.quantile > 0 && opts.quantile < 1)) {
                std::fprintf(stderr, "--quantile needs a number between 0 and 1\n%s", USAGE);
                return false;
            }
        } else if (option == "--min-blocks" && has_value) {
            char *end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || val <= 0) {
                std::fprintf(stderr, "--min-blocks needs a positive integer\n%s", USAGE);
                return false;
            }
            opts.min_blocks = static_cast<int>(val);
        } else if (option == "--json") {
            opts.json = true;
        } else if (option == "--no-color") {
            opts.color = false;
        } else if (option == "--ascii") {
            opts.unicode = false;
        } else if (option == "--color") {
            opts.color = true;
        } else if (option.size() > 0 && option[0] != '-') {
            if (opts.trace.has_value()) {
                std::fprintf(stderr, "Only one trace argument allowed\n%s", USAGE);
                return false;
            }
            opts.trace = fs::u8path(option);
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n%s", option.c_str(), USAGE);
            return false;
        }
    }
    return true;
}

int report_command(int argc, char *argv[]) {
    ReportOptions opts;
    if (!parse_report_options(argc, argv, opts)) return 2;

    fs::path trace_folder;
    if (opts.trace.has_value()) {
        trace_folder = opts.trace.value();
    } else {
        auto latest = latest_trace(opts.root);
        if (!latest.has_value()) {
            fs::path root = opts.root.value_or(default_trace_root());
            std::fprintf(stderr, "No traces in %s. Record one with: sessionscope record, or pass a folder.\n",
                         display_text(root).c_str());
            return 1;
        }
        trace_folder = latest.value();
    }

    TraceReader trace(trace_folder);
    try {
        trace.update();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Error loading trace: %s\n", e.what());
        return 1;
    }

    Style style(opts.color, opts.unicode);

    try {
        if (opts.json) {
            JsonValue json = report_json(trace, opts.metric, opts.min_blocks, opts.quantile);
            std::string text = json_text(json, 2);
            std::printf("%s\n", text.c_str());
        } else {
            std::string text = report_text(trace, opts.metric, opts.min_blocks, opts.quantile, &style);
            std::printf("%s\n", text.c_str());
        }
    } catch (const std::runtime_error &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Error generating report: %s\n", e.what());
        return 1;
    }

    return 0;
}

// The offline bench (decision 0006): every plug-in, named on the command line or found in a
// REAPER project (--project), is loaded in its own bench host process (bench_runner.h), timed, and
// checked for really processing. From a project, each plug-in gets the state saved in it: the
// person's own settings, whatever preset format the plug-in uses (rpp_reader.h). Prints one line
// per plug-in; --json prints the hosts' results as they wrote them, plus how each run ended.
// Times are shown only for plug-ins that passed the engagement check: a plug-in that did nothing
// gets no number (docs/what-failed.md: a wrong number that looks right is the worst result).
int bench_command(int argc, char *argv[]) {
    BenchRequest request;
    std::vector<fs::path> plugins;
    fs::path out = fs::temp_directory_path() / "sessionscope-bench", project;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        try {
            if (option == "--json") json = true;
            else if (option == "--parameters") request.report_parameters = true;
            else if (option == "--project" && has_value) project = fs::u8path(argv[++i]);
            else if (option == "--class" && has_value) request.class_name = argv[++i];
            else if (option == "--preset" && has_value) request.preset = fs::u8path(argv[++i]);
            else if (option == "--program" && has_value) request.program = std::stoi(argv[++i]);
            else if (option == "--rate" && has_value) request.rate = std::stod(argv[++i]);
            else if (option == "--block" && has_value) request.block = std::stoi(argv[++i]);
            else if (option == "--seconds" && has_value) request.seconds = std::stod(argv[++i]);
            else if (option == "--timeout" && has_value) request.timeout_seconds = std::stod(argv[++i]);
            else if (option == "--out" && has_value) out = fs::u8path(argv[++i]);
            else if (option.rfind("--", 0) == 0) {
                std::fprintf(stderr, "Unknown option: %s\n%s", option.c_str(), USAGE);
                return 2;
            } else plugins.push_back(fs::u8path(option));
        } catch (const std::exception &) {
            std::fprintf(stderr, "Not a number after %s\n", option.c_str());
            return 2;
        }
    }
    if (plugins.empty() == project.empty()) {
        std::fprintf(stderr, "Name .vst3 files, or one project with --project.\n%s", USAGE);
        return 2;
    }
    if (plugins.size() > 1 && !request.preset.empty()) {
        std::fprintf(stderr, "--preset belongs to one plug-in: bench one plug-in at a time with it.\n");
        return 2;
    }
    const fs::path host = bench_host_near_program();
    if (host.empty() || !fs::exists(host)) {
        std::fprintf(stderr, "Can't find the bench host (build/native/sessionscope_bench_host.exe). Build first.\n");
        return 1;
    }

    // What to run: one job per plug-in. A job with a `skip` reason is listed but not run.
    struct Job {
        std::string label;       // what the table shows before the bench's own name is known
        BenchRequest request;
        std::string skip;
    };
    std::vector<Job> jobs;
    for (const fs::path &plugin : plugins) {
        Job job{plugin.stem().u8string(), request, ""};
        job.request.plugin = plugin;
        jobs.push_back(job);
    }
    if (!project.empty()) {
        std::vector<ProjectPlugin> found;
        try {
            // REAPER (.rpp, text) or Ableton Live (.als, gzip XML).
            const bool live = lower_extension(project) == ".als";
            found = live ? read_als_plugins(project) : read_rpp_plugins(project);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
        std::error_code ignored;
        fs::create_directories(out / "state", ignored);
        for (std::size_t n = 0; n < found.size(); ++n) {
            const ProjectPlugin &p = found[n];
            Job job{(p.track.empty() ? "Track " + std::to_string(p.track_number) : p.track) + ": " + p.name, request, ""};
            if (p.offline) job.skip = "offline in the project";
            else if (p.kind != "VST3") job.skip = "not measured: " + p.kind + " plug-ins are not hosted yet";
            else if (p.bypassed) job.skip = "bypassed in the project (costs about nothing)";
            else if (!p.problem.empty()) job.skip = "not measured: " + p.problem;
            else if ((job.request.plugin = p.file.empty() ? find_vst3_by_id(host, p.class_id, p.name, default_vst3_index(), out / "scan")
                                                           : find_vst3(p.file)).empty())
                job.skip = "not installed here: " + (p.file.empty() ? p.name : p.file);
            else {
                // The two halves of the saved state, as files the bench host reads.
                const fs::path base = out / "state" / std::to_string(n + 1);
                job.request.class_id = p.class_id;
                job.request.state_component = fs::path(base).concat(".component");
                std::ofstream(job.request.state_component, std::ios::binary) << p.component;
                if (!p.controller.empty()) {
                    job.request.state_controller = fs::path(base).concat(".controller");
                    std::ofstream(job.request.state_controller, std::ios::binary) << p.controller;
                }
            }
            jobs.push_back(job);
        }
    }

    if (!json) {
        std::printf("Offline bench: %g Hz, %d-sample blocks (%.2f ms), %g s each. Share = time in the plug-in / block length.\n\n",
                    request.rate, request.block, request.block * 1000.0 / request.rate, request.seconds);
        std::printf("%-40s %-10s %-20s %8s %8s %8s %8s\n", "Plug-in", "Kind", "Check", "Mean", "p99", "Max", "Mean us");
    }
    std::vector<JsonValue> results;
    for (std::size_t n = 0; n < jobs.size(); ++n) {
        const Job &job = jobs[n];
        if (!job.skip.empty()) {
            if (json) {
                results.emplace_back(std::vector<std::pair<std::string, JsonValue>>{
                    {"plugin", JsonValue(job.label)}, {"outcome", JsonValue("skipped")}, {"reason", JsonValue(job.skip)}});
            } else {
                std::printf("%-40.40s %-10s %s\n", job.label.c_str(), "", job.skip.c_str());
            }
            continue;
        }
        const std::string tag = std::to_string(n + 1) + "-" + job.request.plugin.stem().u8string();
        const BenchRun run = run_bench(host, job.request, out, tag);
        const std::optional<JsonValue> result = parse_json(run.result_json);
        const JsonValue none;
        const JsonValue &r = result ? *result : none;

        if (json) {
            results.emplace_back(std::vector<std::pair<std::string, JsonValue>>{
                {"plugin", JsonValue(job.label)},
                {"outcome", JsonValue(run.outcome)},
                {"exit_code", JsonValue(static_cast<double>(run.exit_code))},
                {"last_stage", JsonValue(run.last_stage)},
                {"log", JsonValue(run.log.u8string())},
                {"result", r}});
            continue;
        }

        // From a project the track names the plug-in; otherwise the bench's name for it.
        const std::string name = !project.empty() || !r["name"].is_string() ? job.label : r["name"].text();
        const std::string kind = r["kind"].is_string() ? r["kind"].text() : "";
        std::string check;
        if (run.outcome == "timed out") check = "timed out at " + run.last_stage;
        else if (run.outcome == "could not start") check = "not started: " + run.last_stage;
        else if (r["status"].is_string() && r["status"].text() == "failed") check = "failed: " + r["error"].text();
        else if (!r["engagement"].is_string()) {
            char code[16];
            std::snprintf(code, sizeof code, "0x%08lX", run.exit_code);
            check = "crashed at " + run.last_stage + " (" + code + ")";
        } else {
            check = r["engagement"].text();
            // It passed the sound through at the settings it was given and changed it only once
            // its parameters were moved: it runs, but the times are of those idle settings.
            if (check == "engaged-after-nudge") check = "runs, idle settings";
            if (run.outcome == "crashed") check += ", crashed on unload";
        }
        const bool engaged = r["engagement"].is_string() && r["engagement"].text().rfind("engaged", 0) == 0;
        if (engaged) {
            const JsonValue &share = r["share_of_block"];
            std::printf("%-40.40s %-10s %-20.20s %7.2f%% %7.2f%% %7.2f%% %8.1f\n", name.c_str(), kind.c_str(), check.c_str(),
                        share["mean"].number() * 100, share["p99"].number() * 100, share["max"].number() * 100,
                        r["time_us"]["mean"].number());
        } else {
            std::printf("%-40.40s %-10s %s\n", name.c_str(), kind.c_str(), check.c_str());
        }
    }
    if (json) {
        std::printf("%s\n", json_text(JsonValue(std::move(results)), 2).c_str());
    } else {
        std::printf("\nResults and logs: %s\n", out.u8string().c_str());
    }
    return 0;
}

// What the HUD's Task Manager shows, as text: every running SessionScope Probe, grouped by track,
// with the share of the audio block's time its plug-in used over one second. Watches the probes'
// shared table for that second, then prints. --json prints the same as data, for scripts.
int probes_command(int argc, char *argv[]) {
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json = true;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n%s", argv[i], USAGE);
            return 2;
        }
    }

    ProbeMonitor monitor(probes::TABLE_NAME, false);   // false: only look, leave the peaks to the HUD
    for (int reading = 0; reading < 11; ++reading) {   // 11 readings, 100 ms apart: one second of differences
        if (reading > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        monitor.tick();
    }

    if (json) {
        std::vector<JsonValue> tracks;
        for (const TrackLoad &track : monitor.tracks()) {
            std::vector<JsonValue> plugins;
            for (const PluginLoad &plugin : track.plugins) {
                plugins.emplace_back(std::vector<std::pair<std::string, JsonValue>>{
                    {"name", JsonValue(plugin.name)},
                    {"cpu", JsonValue(plugin.cpu)},
                    {"peak", JsonValue(plugin.peak)},
                    {"blocks", JsonValue(static_cast<double>(plugin.blocks))},
                    {"process_id", JsonValue(static_cast<double>(plugin.process_id))}});
            }
            tracks.emplace_back(std::vector<std::pair<std::string, JsonValue>>{
                {"name", JsonValue(track.name)},
                {"order", JsonValue(static_cast<double>(track.order))},
                {"cpu", JsonValue(track.cpu)},
                {"plugins", JsonValue(std::move(plugins))}});
        }
        const JsonValue report(std::vector<std::pair<std::string, JsonValue>>{{"tracks", JsonValue(std::move(tracks))}});
        std::printf("%s\n", json_text(report, 2).c_str());
        return 0;
    }

    if (!monitor.any()) {
        std::printf("No SessionScope Probe is running. Put 'SessionScope Probe' on a track, pick a plug-in in it, and play.\n");
        return 0;
    }
    std::printf("%-40s %8s %8s %12s\n", "Track / plug-in", "CPU", "Peak", "Blocks");
    for (const TrackLoad &track : monitor.tracks()) {
        std::printf("%-40.40s %7.2f%% %7.2f%%\n", track.name.c_str(), track.cpu * 100, track.peak * 100);
        for (const PluginLoad &plugin : track.plugins) {
            std::printf("  %-38.38s %7.2f%% %7.2f%% %12llu\n", plugin.name.c_str(), plugin.cpu * 100, plugin.peak * 100,
                        static_cast<unsigned long long>(plugin.blocks));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   // trace folders may have non-ASCII names
#endif
    try {
        const std::string command = argc > 1 ? argv[1] : "";
        if (command == "record") return record_command(argc, argv);
        if (command == "list") return list_command(argc, argv);
        if (command == "report") return report_command(argc, argv);
        if (command == "simulate") return simulate_command(argc, argv);
        if (command == "probes") return probes_command(argc, argv);
        if (command == "bench") return bench_command(argc, argv);
        if (command == "install") return install_command(argc, argv);
        if (command == "reaper") return reaper_command(argc, argv);
        if (command == "--version") {
            std::printf("sessionscope 0.1.0\n");
            return 0;
        }
        if (command == "--help") {
            std::printf("%s", USAGE);
            return 0;
        }
        std::fprintf(stderr, "%s", USAGE);
        return 2;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
}
