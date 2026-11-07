// TraceRecorder implementation. The trace format is in docs/decisions/0004-trace-format.md.

#include "trace_recorder.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <system_error>

#include "blocks.h"
#include "json.h"
#include "sensor_ring.h"

namespace sessionscope {

namespace {

constexpr const char *RECORDER_VERSION = "0.1.0";

// Appends the number in its shortest form that reads back exactly. NaN appends nothing: an empty
// cell is how a trace says "unknown", and missing data must never look like a value.
void append_number(std::string &text, double value) {
    if (std::isnan(value)) return;
    char digits[32];
    const auto written = std::to_chars(digits, digits + sizeof(digits), value);
    text.append(digits, written.ptr);
}

std::string number_text(double value) {
    std::string text;
    append_number(text, value);
    return text.empty() ? "null" : text;   // JSON has no NaN
}

std::string utc_now_iso() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

const char *os_name() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

void write_header(std::ofstream &file, const std::vector<std::string> &columns) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) file << ',';
        file << columns[i];
    }
    file << '\n';
}

}  // namespace

TraceRecorder::TraceRecorder(const std::filesystem::path &folder, SharedSensor &sensor)
    : folder_(folder), sensor_(sensor) {}

TraceRecorder::~TraceRecorder() { finish(); }

void TraceRecorder::poll() {
    if (failed_ || finished_) return;

    if (sensor_.poll() > 0) {
        if (!started_) start_recording();
        if (failed_) return;
        append_new_sensor_rows();
    }
    if (!started_ || failed_) return;

    if (sensor_.second_sensor()) second_sensor_seen_ = true;

    // Outside the "new rows" branch on purpose: when a host stops calling the plug-in, data must
    // still reach the disk, and the DAW's CPU use is still worth knowing.
    const double now = now_seconds();
    if (now - last_second_work_ >= 1.0) do_once_per_second_work(now);
}

void TraceRecorder::start_recording() {
    std::error_code error;
    std::filesystem::create_directories(folder_, error);
    if (error) {
        failed_ = true;
        return;
    }

    sensor_csv_.open(folder_ / "sensor.csv", std::ios::binary);
    events_csv_.open(folder_ / "events.csv", std::ios::binary);
    process_csv_.open(folder_ / "process.csv", std::ios::binary);
    if (!sensor_csv_ || !events_csv_ || !process_csv_) {
        failed_ = true;
        return;
    }

    write_header(sensor_csv_, SENSOR_COLUMNS);
    write_header(events_csv_, EVENT_COLUMNS);
    process_csv_ << "t,cpu_share\n";

    const Table &sensor = sensor_.sensor();
    for (const std::string &column : SENSOR_COLUMNS) sensor_columns_.push_back(&sensor[column]);

    t_start_ = now_seconds();
    last_second_work_ = t_start_;
    started_utc_ = utc_now_iso();
    first_sample_rate_ = sensor["srate"][0];
    first_block_frames_ = sensor["frames"][0];

    process_cpu_.follow(sensor_.host_process_id());
    host_name_ = process_cpu_.executable_name();
    process_cpu_.sample();   // the baseline for the first reading

    events_csv_ << number_text(t_start_) << ",recorder_start," << "recorder " << RECORDER_VERSION << '\n';
    write_meta_json(false);
    started_ = true;
}

void TraceRecorder::append_new_sensor_rows() {
    const std::size_t total_rows = sensor_.sensor().size();
    row_text_.clear();
    for (; rows_written_ < total_rows; ++rows_written_) {
        for (std::size_t column = 0; column < sensor_columns_.size(); ++column) {
            if (column) row_text_.push_back(',');
            append_number(row_text_, (*sensor_columns_[column])[rows_written_]);
        }
        row_text_.push_back('\n');
    }
    sensor_csv_.write(row_text_.data(), static_cast<std::streamsize>(row_text_.size()));
    if (!sensor_csv_) failed_ = true;
}

void TraceRecorder::do_once_per_second_work(double now) {
    last_second_work_ = now;

    // follow() does nothing unless another DAW has taken over the ring. Keep the last name known:
    // the DAW may well close before the recording stops, and the trace still came from it.
    process_cpu_.follow(sensor_.host_process_id());
    if (!process_cpu_.executable_name().empty()) host_name_ = process_cpu_.executable_name();
    const double cpu_share = process_cpu_.sample();
    if (!std::isnan(cpu_share)) process_csv_ << number_text(now) << ',' << number_text(cpu_share) << '\n';

    sensor_csv_.flush();
    events_csv_.flush();
    process_csv_.flush();
    if (!sensor_csv_ || !events_csv_ || !process_csv_) failed_ = true;
}

void TraceRecorder::write_meta_json(bool final) {
    // Written to one side and renamed, so a reader never sees half a file.
    const std::filesystem::path temporary = folder_ / "meta.json.tmp";
    std::ofstream meta(temporary, std::ios::binary);

    meta << "{\n"
         << "  \"schema\": \"sessionscope.trace\",\n"
         << "  \"schema_version\": 1,\n"
         << "  \"recorder_version\": " << json_escaped(RECORDER_VERSION) << ",\n"
         << "  \"started_utc\": " << json_escaped(started_utc_) << ",\n"
         << "  \"t_start\": " << number_text(t_start_) << ",\n"
         << "  \"clock\": \"now_seconds (seconds); sensor and recorder share it\",\n"
         << "  \"host\": {\"name\": " << json_escaped(host_name_.empty() ? "unknown" : host_name_)
         << ", \"os\": " << json_escaped(os_name()) << "},\n"
         << "  \"device\": {\"mode\": \"sensor\", \"srate\": " << number_text(first_sample_rate_)
         << ", \"bsize\": " << number_text(first_block_frames_) << "},\n"
         << "  \"project\": {\"name\": null}";

    if (final) {
        const Blocks blocks = build_blocks(sensor_.sensor());
        meta << ",\n"
             << "  \"stopped_utc\": " << json_escaped(utc_now_iso()) << ",\n"
             << "  \"duration_s\": " << number_text(now_seconds() - t_start_) << ",\n"
             << "  \"summary\": {\n"
             << "    \"sensor_records\": " << rows_written_ << ",\n"
             << "    \"transport_samples\": 0,\n"
             << "    \"passes\": " << blocks.passes().size() << ",\n"
             << "    \"dropped_records\": " << sensor_.dropped() << ",\n"
             << "    \"ring_resets\": " << blocks.resets() << ",\n"
             // A plug-in cannot see the audio driver's xruns, so 0 here means "not measured".
             << "    \"audio_xruns\": 0,\n"
             << "    \"media_xruns\": 0,\n"
             << "    \"sensor_connected\": true,\n"
             << "    \"second_sensor\": " << (second_sensor_seen_ ? "true" : "false") << "\n"
             << "  }";
    }
    meta << "\n}\n";
    meta.close();

    std::error_code error;
    if (meta.fail()) failed_ = true;
    else std::filesystem::rename(temporary, folder_ / "meta.json", error);
    if (error) failed_ = true;
}

void TraceRecorder::finish() {
    if (finished_) return;
    finished_ = true;
    if (!started_) return;   // nothing was recorded, so there is nothing to close

    events_csv_ << number_text(now_seconds()) << ",recorder_stop,\n";
    sensor_csv_.close();
    events_csv_.close();
    process_csv_.close();
    write_meta_json(true);
}

}  // namespace sessionscope
