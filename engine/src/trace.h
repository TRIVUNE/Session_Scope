#ifndef SESSIONSCOPE_TRACE_H
#define SESSIONSCOPE_TRACE_H

// Reads recorded traces: sensor.csv, transport.csv, events.csv, host.csv and meta.json in one
// folder. Keeps reading as a recorder appends, so the same code serves finished and live traces.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sessionscope {

// Column names, in file order.
inline const std::vector<std::string> SENSOR_COLUMNS = {
    "seq", "instance", "t", "frames", "srate", "play_state", "play_pos", "qn", "tempo", "ts_num", "ts_den",
    "self_us", "r_measure", "r_beat", "r_qn", "workload_us"
};

inline const std::vector<std::string> TRANSPORT_COLUMNS = {
    "t", "pass", "play_state", "pos", "pos2", "cursor", "rate", "repeat", "loop_start", "loop_end",
    "tempo", "ts_num", "ts_den", "measure", "beat", "state_count"
};

inline const std::vector<std::string> EVENT_COLUMNS = { "t", "kind", "detail" };

inline const std::vector<std::string> HOST_COLUMNS = { "t", "dur_us", "frames", "srate", "lost" };


// Named columns of doubles. Hot loops take a reference to one column once and index it directly.
class Table {
public:
    explicit Table(const std::vector<std::string> &names);

    std::size_t size() const;  // number of rows

    // A column by name; throws std::out_of_range if there is no such column.
    const std::vector<double> &operator[](std::string_view name) const;

    void append_row(const std::vector<double> &values);

    // Removes the first `rows` rows (all of them if there are fewer). For live data that only
    // needs its recent past: the row after them becomes row 0.
    void drop_front(std::size_t rows);

private:
    std::vector<std::string> names_;
    std::vector<std::vector<double>> columns_;  // parallel to names_
};


// A CSV cell as a number. Empty or unreadable becomes NaN: missing data must never look like 0.
double parse_number(std::string_view text);


// Reads the complete rows appended to a CSV file since the last call. A row the writer has not
// finished is left for the next call. Follows Python's csv module for quoted cells: commas,
// doubled quotes and newlines inside quotes. The first row is the header; expected columns are
// found by name, so extra or reordered columns still load.
class CsvFollower {
public:
    using RowCallback = std::function<void(const std::vector<std::string_view> &cells)>;

    CsvFollower(const std::filesystem::path &path, const std::vector<std::string> &expected_columns);

    // The fast path: calls on_row for each new data row with its cells in file order. The cells
    // are only valid during the call. Throws std::runtime_error if an expected column is missing.
    void for_each_new_row(const RowCallback &on_row);

    // The convenient path: new rows as strings, holding only the expected columns, in that order.
    std::vector<std::vector<std::string>> read();

    std::size_t header_size() const { return header_size_; }
    const std::vector<std::size_t> &index() const { return index_; }   // expected column -> file column

private:
    void read_header(const std::vector<std::string_view> &cells);

    std::filesystem::path path_;
    std::vector<std::string> expected_;
    std::size_t offset_ = 0;           // bytes already handled
    bool header_read_ = false;
    std::size_t header_size_ = 0;
    std::vector<std::size_t> index_;
};


// One row of events.csv.
struct Event {
    double t;
    std::string kind;
    std::string detail;
};


// One trace folder, read incrementally.
class TraceReader {
public:
    explicit TraceReader(const std::filesystem::path &folder);

    // Reads whatever is new in all files. Returns the number of new sensor rows.
    std::size_t update();

    const Table &sensor() const { return sensor_; }
    const Table &transport() const { return transport_; }
    const Table &host() const { return host_; }
    const std::vector<Event> &events() const { return events_; }
    const std::string &meta_json() const { return meta_json_; }
    const std::filesystem::path &folder() const { return folder_; }

private:
    static void append_new_rows(CsvFollower &follower, Table &table);

    std::filesystem::path folder_;
    Table sensor_;
    Table transport_;
    Table host_;
    std::vector<Event> events_;
    std::string meta_json_;

    CsvFollower sensor_follower_;
    CsvFollower transport_follower_;
    CsvFollower host_follower_;
    CsvFollower events_follower_;
};


// Loads a whole trace folder. Throws std::runtime_error if it has no sensor.csv.
TraceReader load_trace(const std::filesystem::path &folder);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_TRACE_H
