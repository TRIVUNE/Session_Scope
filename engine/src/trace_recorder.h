#ifndef SESSIONSCOPE_TRACE_RECORDER_H
#define SESSIONSCOPE_TRACE_RECORDER_H

// Records a running sensor into a trace folder: sensor.csv, events.csv, process.csv and meta.json.
// Works in any DAW, because all it needs is the sensor. This runs for hours, so writing a row
// allocates nothing once the buffers have grown.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "process_cpu.h"
#include "ring_reader.h"

namespace sessionscope {

class TraceRecorder {
public:
    // Creates nothing yet: a run that never sees a sensor leaves no empty folder behind.
    TraceRecorder(const std::filesystem::path &folder, SharedSensor &sensor);
    ~TraceRecorder();
    TraceRecorder(const TraceRecorder &) = delete;
    TraceRecorder &operator=(const TraceRecorder &) = delete;

    // Call often. Appends new sensor rows; once a second, also flushes to disk and samples the CPU.
    void poll();

    // Writes the final meta.json and closes the files. Safe to call twice, and safe when nothing
    // was recorded. The destructor calls it.
    void finish();

    std::size_t rows_written() const { return rows_written_; }
    bool started() const { return started_; }
    bool failed() const { return failed_; }   // a write failed, such as on a full disk: recording has stopped
    const std::filesystem::path &folder() const { return folder_; }

private:
    void start_recording();
    void append_new_sensor_rows();
    void do_once_per_second_work(double now);
    void write_meta_json(bool final);

    std::filesystem::path folder_;
    SharedSensor &sensor_;
    ProcessCpu process_cpu_;

    std::ofstream sensor_csv_;
    std::ofstream events_csv_;
    std::ofstream process_csv_;

    std::vector<const std::vector<double> *> sensor_columns_;   // in file order, found once
    std::string row_text_;                                      // reused between polls

    bool started_ = false;
    bool finished_ = false;
    bool failed_ = false;
    bool second_sensor_seen_ = false;
    std::size_t rows_written_ = 0;

    double t_start_ = 0;
    double last_second_work_ = 0;
    std::string started_utc_;
    std::string host_name_;
    double first_sample_rate_ = 0;
    double first_block_frames_ = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_TRACE_RECORDER_H
