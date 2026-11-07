// The trace simulator. The order of random-number calls decides every value that follows, so it
// must not change: the same seed has to keep producing the same trace.

#include "simulate.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>

#include "json.h"
#include "python_random.h"
#include "text_format.h"
#include "trace.h"

namespace fs = std::filesystem;

namespace sessionscope {

namespace {

constexpr int INSTANCE_ID = 4242;
constexpr double START_TIME = 5000.0;          // an arbitrary recorder clock start
constexpr double TRANSPORT_INTERVAL = 1.0 / 30;

// A 64-bar song: light start, busier middle, a heavy stretch at 33-48 peaking at 41-43.
double song_load(int bar) {
    if (bar <= 16) return 0.35;
    if (bar <= 32) return 0.55;
    if (bar <= 48) return 0.82 + (bar >= 41 && bar <= 43 ? 0.13 : 0.0);
    return 0.40;
}

// The HUD's demo: 16 bars that heat up within ten seconds, so a first look shows what the HUD is
// for. Calm bars 1-2, busier 3-4, a heavy stretch at 5-8 peaking at bar 7, then light again.
double demo_load(int bar) {
    if (bar <= 2) return 0.35;
    if (bar <= 4) return 0.60;
    if (bar <= 8) return bar == 7 ? 0.97 : 0.86;
    return 0.40;
}

// Fixture F7: +500 us per 256 samples every two bars. BLOCK_US is that block at 48 kHz.
const double BLOCK_US = 256.0 / 48000.0 * 1e6;

double staircase_workload(int bar) { return 500.0 * ((bar - 1) / 2) / BLOCK_US; }

double staircase_load(int bar) { return 0.05 + 500.0 * ((bar - 1) / 2) / BLOCK_US; }

const std::vector<Scenario> &scenarios() {
    static const std::vector<Scenario> all = {
        // Played once through, then bars 33-48 three more times (the last two as loop wraps).
        {"song", 64, {{1, 120}, {17, 128}, {49, 100}},
         {{1, 65, "start"}, {33, 49, "start"}, {33, 49, "loop"}, {33, 49, "loop"}},
         song_load, nullptr},
        {"staircase", 24, {{1, 120}}, {{1, 25, "start"}, {1, 25, "loop"}}, staircase_load, staircase_workload},
        // Played through once, then the hot bars 5-8 looped twice: three passes over them.
        {"demo", 16, {{1, 120}}, {{1, 17, "start"}, {5, 9, "loop"}, {5, 9, "loop"}}, demo_load, nullptr},
    };
    return all;
}

// Bar start times for a 4/4 song whose tempo changes at given bars.
class TempoMap {
public:
    TempoMap(const std::vector<std::pair<int, int>> &changes, int bars) {
        const std::map<int, int> changes_by_bar(changes.begin(), changes.end());
        int current_bpm = changes.front().second;
        starts_.push_back(0.0);
        for (int bar = 1; bar <= bars + 1; ++bar) {
            const auto change = changes_by_bar.find(bar);
            if (change != changes_by_bar.end()) current_bpm = change->second;
            bpm_.push_back(current_bpm);
            starts_.push_back(starts_.back() + 4.0 * 60.0 / current_bpm);
        }
    }

    double bar_time(int bar) const { return starts_[static_cast<std::size_t>(bar) - 1]; }

    struct Position {
        int measure;   // 0-based
        double beat;   // beat within the measure
        double quarter_notes;
        int bpm;
    };

    Position at(double time) const {
        std::size_t measure = 0;
        while (measure + 1 < starts_.size() - 1 && starts_[measure + 1] <= time) ++measure;
        const double seconds_per_quarter = 60.0 / bpm_[measure];
        const double beat = (time - starts_[measure]) / seconds_per_quarter;
        return {static_cast<int>(measure), beat, 4.0 * static_cast<double>(measure) + beat, bpm_[measure]};
    }

private:
    std::vector<double> starts_;   // starts_[b - 1] is when bar b begins
    std::vector<int> bpm_;         // bpm_[b - 1] is the tempo of bar b
};

// One CSV file being written, built from cells. Whole numbers and floats look different in a trace
// ("256" and "256.0"), so each has its own function.
class CsvWriter {
public:
    CsvWriter(const fs::path &path, const std::vector<std::string> &header) : file_(path, std::ios::binary) {
        for (const std::string &column : header) text(column);
        end_row();
    }

    CsvWriter &whole(int value) { return text(std::to_string(value)); }
    CsvWriter &real(double value) { return text(float_text(value)); }
    CsvWriter &real(const std::optional<double> &value) { return value ? real(*value) : text(""); }   // unknown: empty

    CsvWriter &text(const std::string &value) {
        if (!row_.empty() || cells_in_row_ > 0) row_ += ',';
        ++cells_in_row_;
        const bool needs_quotes = value.find_first_of(",\"\n") != std::string::npos;
        if (!needs_quotes) {
            row_ += value;
            return *this;
        }
        row_ += '"';
        for (const char c : value) {
            if (c == '"') row_ += '"';
            row_ += c;
        }
        row_ += '"';
        return *this;
    }

    void end_row() {
        row_ += '\n';
        file_.write(row_.data(), static_cast<std::streamsize>(row_.size()));
        row_.clear();
        cells_in_row_ = 0;
    }

private:
    std::ofstream file_;
    std::string row_;
    int cells_in_row_ = 0;
};

class Simulation {
public:
    Simulation(const fs::path &out, const Scenario &scenario, const SimulateOptions &options)
        : out_(out),
          scenario_(scenario),
          options_(options),
          tempo_(scenario.tempo, scenario.bars),
          random_(options.seed),
          host_random_(options.seed + 1),   // separate, so adding host.csv leaves the sensor data unchanged
          block_seconds_(static_cast<double>(options.frames) / options.srate),
          time_(START_TIME),
          next_transport_sample_(START_TIME),
          sensor_csv_(out / "sensor.csv", SENSOR_COLUMNS),
          transport_csv_(out / "transport.csv", TRANSPORT_COLUMNS),
          events_csv_(out / "events.csv", EVENT_COLUMNS) {
        if (options.host) host_csv_.emplace(out / "host.csv", HOST_COLUMNS);
    }

    void run() {
        add_event("recorder_start", "synthetic");
        add_event("sensor", "sensor v0.1 connected");
        const std::vector<PlanPart> &plan = scenario_.plan;
        for (std::size_t part = 0; part < plan.size(); ++part) play_part(part);

        // Half a second stopped at the end.
        const int final_blocks = static_cast<int>(0.5 / block_seconds_);
        const double end_position = tempo_.bar_time(plan.back().end_bar);
        for (int i = 0; i < final_blocks; ++i) block(false, end_position, false, 0.0, 0.0);
        add_event("recorder_stop", "");
    }

    void write_meta() const {
        using Object = std::vector<std::pair<std::string, JsonValue>>;
        const auto number = [](auto value) { return JsonValue(static_cast<double>(value)); };
        const JsonValue meta(Object{
            {"schema", JsonValue(std::string("sessionscope.trace"))},
            {"schema_version", number(1)},
            {"synthetic", JsonValue(true)},
            {"scenario", JsonValue(scenario_.name)},
            {"seed", number(options_.seed)},
            {"recorder_version", JsonValue(std::string("simulate"))},
            {"t_start", number(START_TIME)},
            {"host", JsonValue(Object{{"name", JsonValue(std::string("simulated"))},
                                      {"version", JsonValue(std::string("-"))},
                                      {"os", JsonValue(std::string("-"))}})},
            {"device", JsonValue(Object{{"mode", JsonValue(std::string("simulated"))},
                                        {"srate", number(options_.srate)},
                                        {"bsize", number(options_.frames)}})},
            {"project", JsonValue(Object{{"name", JsonValue(scenario_.name + " (synthetic)")}})},
            {"duration_s", number(time_ - START_TIME)},
            {"summary", JsonValue(Object{{"sensor_records", number(sensor_records_)},
                                         {"dropped_records", number(lost_records_)},
                                         {"audio_xruns", number(xruns_)},
                                         {"passes", number(scenario_.plan.size())},
                                         {"sensor_connected", JsonValue(true)}})},
        });
        std::ofstream file(out_ / "meta.json", std::ios::binary);
        file << json_text(meta, 2) << '\n';
    }

private:
    // A second of stopped transport before each fresh start, none before a loop wrap.
    void play_part(std::size_t part) {
        const std::vector<PlanPart> &plan = scenario_.plan;
        const PlanPart &current = plan[part];
        const bool next_is_loop = part + 1 < plan.size() && plan[part + 1].kind == "loop";
        const bool repeat_on = current.kind == "loop" || next_is_loop;
        const double loop_start = repeat_on ? tempo_.bar_time(current.first_bar) : 0.0;
        const double loop_end = repeat_on ? tempo_.bar_time(current.end_bar) : 0.0;

        const double start_position = tempo_.bar_time(current.first_bar);
        if (current.kind == "start") {
            const int stopped_blocks = static_cast<int>(1.0 / block_seconds_);
            for (int i = 0; i < stopped_blocks; ++i) block(false, start_position, repeat_on, loop_start, loop_end);
        }

        ++pass_number_;
        add_event(current.kind, "bar " + std::to_string(current.first_bar));

        const double end_position = tempo_.bar_time(current.end_bar);
        for (double position = start_position; position < end_position - 1e-9; position += block_seconds_) {
            block(true, position, repeat_on, loop_start, loop_end);
        }
        if (!next_is_loop) add_event("stop", "bar " + std::to_string(current.end_bar));
    }

    // One audio block at a project position.
    void block(bool playing, double position, bool repeat_on, double loop_start, double loop_end) {
        const TempoMap::Position at = tempo_.at(position);
        const int bar = at.measure + 1;
        const double load = playing ? scenario_.load(bar) : 0.05;

        const double late = invent_lateness(load);
        if (playing && load > 0.9 && late > 0.5 * block_seconds_ && random_.random() < 0.02) {
            ++xruns_;
            add_event("audio_xrun", "the host reported an audio underrun");
        }

        write_sensor_row(playing, position, at);
        if (options_.host) write_host_row(playing ? load : 0.03);
        write_transport_rows(playing, position, at, repeat_on, loop_start, loop_end);

        ++sequence_;
        if (options_.reset_at && sequence_ == *options_.reset_at && !restarted_) {
            restarted_ = true;
            sequence_ = 0;
            add_event("ring_reset", "the sensor restarted its ring");
        }
        time_ += block_seconds_ + late;
    }

    // Invented lateness: small jitter always, a long tail once the load passes 80%.
    double invent_lateness(double load) {
        double late = std::fabs(random_.gauss(0, 0.012)) * block_seconds_;
        if (load > 0.8) late += random_.expovariate(1.0 / ((load - 0.8) * 0.6 * block_seconds_));
        return late;
    }

    void write_sensor_row(bool playing, double position, const TempoMap::Position &at) {
        const bool lost = options_.drop_every && sequence_ % options_.drop_every == options_.drop_every - 1;
        if (lost) {
            ++lost_records_;
            return;
        }

        std::optional<double> workload_us;   // stays empty, meaning unknown, without a workload
        if (scenario_.workload) {
            workload_us = playing ? scenario_.workload(at.measure + 1) * block_seconds_ * 1e6 : 0.0;
        }
        sensor_csv_.whole(sequence_).whole(INSTANCE_ID).real(time_).whole(options_.frames).whole(options_.srate)
            .whole(playing ? 1 : 0).real(position).real(at.quarter_notes).whole(at.bpm).whole(4).whole(4)
            .real(rounded_to_decimals(random_.gauss(1.2, 0.15), 3)).whole(at.measure).real(at.beat)
            .real(at.quarter_notes).real(workload_us);
        sensor_csv_.end_row();
        ++sensor_records_;
    }

    // What the optional REAPER extension would time: the session's load, plus a little noise.
    void write_host_row(double load) {
        const double duration_us = load * block_seconds_ * 1e6 * (1 + host_random_.gauss(0, 0.04));
        host_csv_->real(time_).real(std::max(1.0, duration_us)).whole(options_.frames).whole(options_.srate).whole(0);
        host_csv_->end_row();
    }

    void write_transport_rows(bool playing, double position, const TempoMap::Position &at, bool repeat_on,
                              double loop_start, double loop_end) {
        while (next_transport_sample_ <= time_) {
            transport_csv_.real(next_transport_sample_).whole(playing ? pass_number_ : 0).whole(playing ? 1 : 0)
                .real(position - 0.01).real(position).real(position).whole(1).whole(repeat_on ? 1 : 0)
                .real(loop_start).real(loop_end).whole(at.bpm).whole(4).whole(4).whole(at.measure).real(at.beat).whole(1);
            transport_csv_.end_row();
            next_transport_sample_ += TRANSPORT_INTERVAL;
        }
    }

    void add_event(const std::string &kind, const std::string &detail) {
        events_csv_.text(fixed(time_, 6)).text(kind).text(detail);
        events_csv_.end_row();
    }

    fs::path out_;
    const Scenario &scenario_;
    const SimulateOptions &options_;
    TempoMap tempo_;
    PythonRandom random_;
    PythonRandom host_random_;

    double block_seconds_;
    double time_;
    double next_transport_sample_;
    int sequence_ = 0;
    int pass_number_ = 0;
    int xruns_ = 0;
    int lost_records_ = 0;
    int sensor_records_ = 0;
    bool restarted_ = false;

    CsvWriter sensor_csv_;
    CsvWriter transport_csv_;
    CsvWriter events_csv_;
    std::optional<CsvWriter> host_csv_;
};

}  // namespace

const Scenario *scenario_by_name(std::string_view name) {
    for (const Scenario &scenario : scenarios()) {
        if (scenario.name == name) return &scenario;
    }
    return nullptr;
}

std::vector<std::string> scenario_names() {
    std::vector<std::string> names;
    for (const Scenario &scenario : scenarios()) names.push_back(scenario.name);
    return names;
}

fs::path write_simulated_trace(const fs::path &out, const Scenario &scenario, const SimulateOptions &options) {
    fs::create_directories(out);
    {
        Simulation simulation(out, scenario, options);
        simulation.run();
        simulation.write_meta();
    }   // closes the files

    if (options.truncate) {
        const fs::path sensor_file = out / "sensor.csv";
        fs::resize_file(sensor_file, fs::file_size(sensor_file) - 7);
    }
    return out;
}

fs::path write_simulated_trace(const fs::path &out, std::string_view scenario_name, const SimulateOptions &options) {
    const Scenario *scenario = scenario_by_name(scenario_name);
    if (!scenario) throw std::invalid_argument("Unknown scenario: " + std::string(scenario_name));
    return write_simulated_trace(out, *scenario, options);
}

}  // namespace sessionscope
