// The sessionscope command-line program. For now: record a trace from a running sensor, in any DAW,
// and list recorded traces. Built by tools/build_engine.py into build/engine/.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

#include "json.h"
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
    "  sessionscope --version\n"
    "  sessionscope --help\n";

std::atomic<bool> g_stop_requested{false};

void request_stop(int) { g_stop_requested = true; }

std::string display_text(const fs::path &path) { return path.u8string(); }

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
