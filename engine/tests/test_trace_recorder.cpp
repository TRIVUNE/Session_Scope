// Tests for TraceRecorder. Uses a private ring and a temporary folder, never the real ones.

#include "trace_recorder.h"
#include "trace.h"
#include "json.h"
#include "test_support.h"

#include <iterator>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sessionscope {
namespace test {

// A helper that creates a writable mapping of the given name and size,
// and allows writing doubles into it. Used by tests to simulate the writer.
class WritableRing {
public:
    explicit WritableRing(const char *name, size_t bytes)
        : name_(name), bytes_(bytes) {
#ifdef _WIN32
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(bytes_), name_);
        if (!mapping_) return;
        base_ = static_cast<volatile double *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bytes_));
#else
        fd_ = shm_open(name_, O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) return;
        if (ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
            close(fd_);
            fd_ = -1;
            return;
        }
        void *p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            return;
        }
        base_ = static_cast<volatile double *>(p);
#endif
    }

    ~WritableRing() {
        if (base_) {
#ifdef _WIN32
            UnmapViewOfFile(const_cast<void *>(static_cast<const volatile void *>(base_)));
            if (mapping_) CloseHandle(mapping_);
#else
            munmap(const_cast<void *>(static_cast<const volatile void *>(base_)), bytes_);
            if (fd_ >= 0) close(fd_);
            shm_unlink(name_);
#endif
        }
    }

    bool valid() const { return base_ != nullptr; }

    void put(std::size_t slot, double value) {
        if (base_) base_[slot] = value;
    }

    double get(std::size_t slot) const {
        return base_ ? base_[slot] : 0.0;
    }

private:
    const char *name_;
    size_t bytes_;
    volatile double *base_ = nullptr;

#ifdef _WIN32
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Generates a unique test mapping name using the process ID.
std::string test_sensor_name() {
    char buf[64];
#ifdef _WIN32
    std::snprintf(buf, sizeof(buf), "SessionScope_test_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
#else
    std::snprintf(buf, sizeof(buf), "/SessionScope_test_%d", getpid());
#endif
    return std::string(buf);
}

}  // namespace test
}  // namespace sessionscope

using namespace sessionscope;
using namespace sessionscope::test;

// Sensor ring constants from sensor_ring.h
#include "sensor_ring.h"

void test_trace_recorder_no_rows_creates_no_folder() {
    std::string name = test_sensor_name() + "_empty";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    // Initialize header but write no records.
    mm.put(0, MAGIC);
    mm.put(1, LAYOUT);
    mm.put(2, 0.0);
    mm.put(3, static_cast<double>(CAP));
    mm.put(4, static_cast<double>(REC));
    mm.put(5, 77.0);
    mm.put(6, now_seconds());
    mm.put(7, 0.0);

    SharedSensor sensor(name.c_str());
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_empty";

    TraceRecorder recorder(folder, sensor);
    recorder.finish();

    CHECK(!recorder.started());
    CHECK(recorder.rows_written() == 0);
    CHECK(!std::filesystem::exists(folder));

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_writes_500_records_and_loads_back() {
    std::string name = test_sensor_name() + "_500";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    // Initialize header.
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_500";

    TraceRecorder recorder(folder, sensor);

    // Write 500 records in two batches with polls between.
    const int total_records = 500;
    const int batch1 = 200;
    const int batch2 = 300;
    double base_t = now_seconds();
    uint64_t seq = 0;

    auto write_batch = [&](int count, double t_offset) {
        for (int k = 0; k < count; ++k) {
            double t = base_t + t_offset + k * 0.005;
            std::size_t base = HDR + (seq % CAP) * REC;
            double qn = seq * 0.01;
            std::vector<double> vals = {
                t, 256, 48000, 1, seq * 0.005, qn, 120, 4, 4, 1.0,
                static_cast<double>(seq), 77.0, std::floor(qn / 4.0) * 4.0
            };
            for (std::size_t j = 0; j < vals.size(); ++j) {
                put(base + j, vals[j]);
            }
            ++seq;
            put(2, static_cast<double>(seq));
            put(6, t);
        }
    };

    // First poll: initializes sensor (skips history), no data yet.
    recorder.poll();

    write_batch(batch1, 0);
    // Second poll: should read batch1.
    recorder.poll();

    write_batch(batch2, 1.0);  // 1 second later
    // Third poll: should read batch2.
    recorder.poll();

    // Wait a bit for the flush timer to trigger (1 second)
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recorder.poll();

    recorder.finish();

    CHECK(recorder.started());
    CHECK(recorder.rows_written() == total_records);
    CHECK(std::filesystem::exists(folder / "sensor.csv"));
    CHECK(std::filesystem::exists(folder / "events.csv"));
    CHECK(std::filesystem::exists(folder / "process.csv"));
    CHECK(std::filesystem::exists(folder / "meta.json"));

    // Load back with load_trace and verify.
    TraceReader reader = load_trace(folder);
    CHECK(reader.sensor().size() == total_records);

    // Verify values match exactly (shortest round-trip must be lossless).
    const auto &loaded_seq_col = reader.sensor()["seq"];
    const auto &loaded_frames = reader.sensor()["frames"];
    const auto &loaded_srate = reader.sensor()["srate"];
    const auto &loaded_play_state = reader.sensor()["play_state"];
    const auto &loaded_tempo = reader.sensor()["tempo"];

    for (int i = 0; i < total_records; ++i) {
        CHECK_NEAR(loaded_seq_col[i], static_cast<double>(i), 1e-9);
        CHECK_NEAR(loaded_frames[i], 256.0, 1e-9);
        CHECK_NEAR(loaded_srate[i], 48000.0, 1e-9);
        CHECK_NEAR(loaded_play_state[i], 1.0, 1e-9);
        CHECK_NEAR(loaded_tempo[i], 120.0, 1e-9);
    }

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_nan_qn_is_empty_cell() {
    std::string name = test_sensor_name() + "_nanqn";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_nanqn";

    TraceRecorder recorder(folder, sensor);

    // First poll: initializes sensor (skips history).
    recorder.poll();

    // Write one record with NaN qn (slot 5).
    double t = now_seconds();
    std::size_t base = HDR + (0 % CAP) * REC;
    put(base + 0, t);
    put(base + 1, 256);
    put(base + 2, 48000);
    put(base + 3, 1);
    put(base + 4, 0.005);
    put(base + 5, std::numeric_limits<double>::quiet_NaN());  // NaN qn
    put(base + 6, 120);
    put(base + 7, 4);
    put(base + 8, 4);
    put(base + 9, 1.0);
    put(base + 10, 0.0);  // seq
    put(base + 11, 77.0);
    put(base + 12, std::numeric_limits<double>::quiet_NaN());
    put(2, 1.0);
    put(6, t);

    // Second poll: reads the record.
    recorder.poll();
    recorder.finish();

    // Load back and verify qn is NaN.
    TraceReader reader = load_trace(folder);
    CHECK(reader.sensor().size() == 1);
    CHECK(std::isnan(reader.sensor()["qn"][0]));

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_meta_json_has_expected_fields() {
    std::string name = test_sensor_name() + "_meta";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_meta";

    TraceRecorder recorder(folder, sensor);

    // First poll: initializes sensor (skips history).
    recorder.poll();

    // Write 10 records.
    for (int k = 0; k < 10; ++k) {
        double t = now_seconds() + k * 0.005;
        std::size_t base = HDR + (k % CAP) * REC;
        double qn = k * 0.01;
        std::vector<double> vals = {t, 256, 48000, 1, k * 0.005, qn, 120, 4, 4, 1.0,
                                    static_cast<double>(k), 77.0, std::floor(qn / 4.0) * 4.0};
        for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
        put(2, static_cast<double>(k + 1));
        put(6, t);
    }

    // Second poll: reads the records.
    recorder.poll();
    recorder.finish();

    // Parse meta.json
    std::ifstream meta_file(folder / "meta.json");
    std::string meta_text((std::istreambuf_iterator<char>(meta_file)), std::istreambuf_iterator<char>());
    auto meta = sessionscope::parse_json(meta_text);
    CHECK(meta.has_value());

    const sessionscope::JsonValue &root = *meta;
    CHECK(root["schema"].text() == "sessionscope.trace");
    CHECK(root["schema_version"].number() == 1.0);
    CHECK(root["recorder_version"].text() == "0.1.0");
    CHECK(!root["started_utc"].text().empty());
    CHECK(!root["stopped_utc"].text().empty());
    CHECK(root["t_start"].number() > 0);
    CHECK(root["clock"].text() == "now_seconds (seconds); sensor and recorder share it");
    CHECK(root["host"]["name"].text() != "");
    CHECK(root["host"]["os"].text() != "");
    CHECK(root["device"]["mode"].text() == "sensor");
    CHECK(root["device"]["srate"].number() == 48000.0);
    CHECK(root["device"]["bsize"].number() == 256.0);
    CHECK(root["project"]["name"].is_null());
    CHECK(root["duration_s"].number() > 0);

    const sessionscope::JsonValue &summary = root["summary"];
    CHECK(summary["sensor_records"].number() == 10.0);
    CHECK(summary["transport_samples"].number() == 0.0);
    CHECK(summary["passes"].number() >= 1.0);  // At least one pass
    CHECK(summary["dropped_records"].number() == 0.0);
    CHECK(summary["ring_resets"].number() == 0.0);
    CHECK(summary["audio_xruns"].number() == 0.0);
    CHECK(summary["media_xruns"].number() == 0.0);
    CHECK(summary["sensor_connected"].boolean() == true);
    CHECK(summary["second_sensor"].boolean() == false);

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_events_csv_has_recorder_start_and_stop() {
    std::string name = test_sensor_name() + "_events";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_events";

    TraceRecorder recorder(folder, sensor);

    // First poll: initializes sensor (skips history).
    recorder.poll();

    // Write 1 record.
    double t = now_seconds();
    std::size_t base = HDR + (0 % CAP) * REC;
    std::vector<double> vals = {t, 256, 48000, 1, 0.005, 0.01, 120, 4, 4, 1.0, 0.0, 77.0, 0.0};
    for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
    put(2, 1.0);
    put(6, t);

    // The ring names this test process as the host.
    put(10, process_id());

    // Second poll: reads the record.
    recorder.poll();

    // The start time must survive to the final meta.json, however long the recording lasts.
    const auto read_meta = [&] {
        std::ifstream meta_file(folder / "meta.json", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(meta_file)), std::istreambuf_iterator<char>());
        return parse_json(text).value_or(JsonValue());
    };
    const std::string started_utc = read_meta()["started_utc"].text();
    CHECK(!started_utc.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    recorder.finish();
    const JsonValue final_meta = read_meta();
    CHECK(final_meta["started_utc"].text() == started_utc);
    CHECK(final_meta["stopped_utc"].text() != started_utc);
    CHECK(final_meta["host"]["name"].text().find("test_trace_recorder") != std::string::npos);

    // Read events.csv
    std::ifstream events_file(folder / "events.csv");
    std::string line;
    std::getline(events_file, line);  // header
    CHECK(line == "t,kind,detail");

    std::getline(events_file, line);  // recorder_start
    CHECK(line.find("recorder_start") != std::string::npos);
    CHECK(line.find("recorder 0.1.0") != std::string::npos);
    CHECK(parse_number(line.substr(0, line.find(','))) > 0);   // stamped with the real start time

    std::getline(events_file, line);  // recorder_stop
    CHECK(line.find("recorder_stop") != std::string::npos);

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_meta_json_tmp_does_not_remain() {
    std::string name = test_sensor_name() + "_tmp";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_tmp";

    TraceRecorder recorder(folder, sensor);

    // First poll: initializes sensor (skips history).
    recorder.poll();

    double t = now_seconds();
    std::size_t base = HDR + (0 % CAP) * REC;
    std::vector<double> vals = {t, 256, 48000, 1, 0.005, 0.01, 120, 4, 4, 1.0, 0.0, 77.0, 0.0};
    for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
    put(2, 1.0);
    put(6, t);

    // Second poll: reads the record.
    recorder.poll();
    recorder.finish();

    CHECK(!std::filesystem::exists(folder / "meta.json.tmp"));
    CHECK(std::filesystem::exists(folder / "meta.json"));

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

void test_trace_recorder_finish_twice_is_safe() {
    std::string name = test_sensor_name() + "_doublefinish";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor sensor(name.c_str());
    // Do NOT call sensor.poll() here - let the recorder do it.

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "sessionscope_test_trace_recorder";
    std::filesystem::create_directories(temp_dir);
    std::filesystem::path folder = temp_dir / "test_doublefinish";

    TraceRecorder recorder(folder, sensor);

    // First poll: initializes sensor (skips history).
    recorder.poll();

    // Write data AFTER first poll.
    double t = now_seconds();
    std::size_t base = HDR + (0 % CAP) * REC;
    std::vector<double> vals = {t, 256, 48000, 1, 0.005, 0.01, 120, 4, 4, 1.0, 0.0, 77.0, 0.0};
    for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
    put(2, 1.0);
    put(6, t);

    // Second poll: reads the record.
    recorder.poll();
    recorder.finish();
    std::size_t rows1 = recorder.rows_written();

    // Call finish again - should be safe.
    recorder.finish();
    std::size_t rows2 = recorder.rows_written();

    CHECK(rows1 == rows2);
    CHECK(rows1 == 1);

    // Cleanup
    try {
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &) {
        // Ignore cleanup errors on Windows (file may be briefly locked)
    }
}

int main() {
    test_trace_recorder_no_rows_creates_no_folder();
    test_trace_recorder_writes_500_records_and_loads_back();
    test_trace_recorder_nan_qn_is_empty_cell();
    test_trace_recorder_meta_json_has_expected_fields();
    test_trace_recorder_events_csv_has_recorder_start_and_stop();
    test_trace_recorder_meta_json_tmp_does_not_remain();
    test_trace_recorder_finish_twice_is_safe();

    return finish_tests();
}
