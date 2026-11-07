// Tests for the trace reader.
// Creates temporary CSV files, exercises each behaviour, and cleans up.

#include "trace.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using sessionscope::Table;
using sessionscope::TraceReader;
using sessionscope::load_trace;
using sessionscope::parse_number;
using sessionscope::CsvFollower;
using sessionscope::Event;
using sessionscope::SENSOR_COLUMNS;
using sessionscope::TRANSPORT_COLUMNS;
using sessionscope::EVENT_COLUMNS;
using sessionscope::HOST_COLUMNS;


// Helper: write a string to a file (binary mode to preserve \n exactly)
static void write_file(const fs::path &p, const std::string &content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

// Helper: append a string to a file (binary mode)
static void append_file(const fs::path &p, const std::string &content) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    f << content;
}

// Helper: create a unique temp directory
static fs::path make_temp_dir() {
    fs::path base = fs::temp_directory_path() / "sessionscope_test";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    fs::path dir = base / std::to_string(dis(gen));
    fs::create_directories(dir);
    return dir;
}

// Helper: remove a directory tree
static void remove_dir(const fs::path &p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}


int main() {
    fs::path tmp = make_temp_dir();

    // ------------------------------------------------------------
    // parse_number tests
    // ------------------------------------------------------------
    {
        CHECK(std::isnan(parse_number("")));
        CHECK(std::isnan(parse_number("   ")));
        CHECK(std::isnan(parse_number("abc")));
        CHECK(std::isnan(parse_number("1.2.3")));

        CHECK_NEAR(parse_number("0"), 0.0, 1e-12);
        CHECK_NEAR(parse_number("-3.14"), -3.14, 1e-12);
        CHECK_NEAR(parse_number("1e6"), 1e6, 1e-9);
        CHECK_NEAR(parse_number("  42  "), 42.0, 1e-12);  // strtod allows leading space
    }

    // ------------------------------------------------------------
    // Table tests
    // ------------------------------------------------------------
    {
        Table t({"a", "b", "c"});
        CHECK(t.size() == 0);

        t.append_row({1.0, 2.0, 3.0});
        t.append_row({4.0, 5.0, 6.0});
        CHECK(t.size() == 2);

        const auto &col_a = t["a"];
        const auto &col_b = t["b"];
        const auto &col_c = t["c"];
        CHECK_NEAR(col_a[0], 1.0, 1e-12);
        CHECK_NEAR(col_a[1], 4.0, 1e-12);
        CHECK_NEAR(col_b[0], 2.0, 1e-12);
        CHECK_NEAR(col_c[1], 6.0, 1e-12);

        // operator[] throws on unknown
        bool threw = false;
        try {
            (void)t["nonexistent"];
        } catch (const std::out_of_range &) {
            threw = true;
        }
        CHECK(threw);

        // append_row with wrong size throws
        threw = false;
        try {
            t.append_row({1.0, 2.0});
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        CHECK(threw);
    }

    // ------------------------------------------------------------
    // CsvFollower basic read
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "basic.csv";
        write_file(csv, "t,value\n1.0,2.0\n3.0,4.0\n");

        CsvFollower follower(csv, {"t", "value"});
        auto rows = follower.read();

        CHECK(rows.size() == 2);
        CHECK(rows[0].size() == 2);
        CHECK(rows[0][0] == "1.0");
        CHECK(rows[0][1] == "2.0");
        CHECK(rows[1][0] == "3.0");
        CHECK(rows[1][1] == "4.0");

        // Second read returns nothing new
        rows = follower.read();
        CHECK(rows.empty());
    }

    // ------------------------------------------------------------
    // CsvFollower: partial last line waits, then read after append
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "partial.csv";
        write_file(csv, "t,value\n1.0,2.0\n");  // complete line
        // Note: no newline after 3.0,4.0

        CsvFollower follower(csv, {"t", "value"});
        auto rows = follower.read();
        CHECK(rows.size() == 1);
        CHECK(rows[0][0] == "1.0");

        // Append the rest of the partial line plus newline
        append_file(csv, "3.0,4.0\n");
        rows = follower.read();
        CHECK(rows.size() == 1);
        CHECK(rows[0][0] == "3.0");
        CHECK(rows[0][1] == "4.0");
    }

    // ------------------------------------------------------------
    // CsvFollower: offset respected, nothing read twice
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "offset.csv";
        write_file(csv, "t,value\n1,2\n3,4\n5,6\n");

        CsvFollower follower(csv, {"t", "value"});
        auto r1 = follower.read();
        CHECK(r1.size() == 3);

        auto r2 = follower.read();
        CHECK(r2.empty());

        // Append more
        append_file(csv, "7,8\n9,10\n");
        auto r3 = follower.read();
        CHECK(r3.size() == 2);
        CHECK(r3[0][0] == "7");
        CHECK(r3[1][0] == "9");
    }

    // ------------------------------------------------------------
    // CsvFollower: reordered and extra columns
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "reordered.csv";
        write_file(csv, "extra,t,value,more\n99,1.0,2.0,88\n");

        CsvFollower follower(csv, {"t", "value"});
        auto rows = follower.read();

        CHECK(rows.size() == 1);
        CHECK(rows[0][0] == "1.0");  // t
        CHECK(rows[0][1] == "2.0");  // value
    }

    // ------------------------------------------------------------
    // CsvFollower: missing expected column throws
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "missing_col.csv";
        write_file(csv, "t,other\n1,2\n");

        CsvFollower follower(csv, {"t", "value"});
        bool threw = false;
        try {
            follower.read();
        } catch (const std::runtime_error &e) {
            threw = true;
            std::string msg = e.what();
            CHECK(msg.find("missing columns") != std::string::npos);
            CHECK(msg.find("value") != std::string::npos);
        }
        CHECK(threw);
    }

    // ------------------------------------------------------------
    // CsvFollower: missing file returns no rows
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "does_not_exist.csv";
        CsvFollower follower(csv, {"t", "value"});
        auto rows = follower.read();
        CHECK(rows.empty());
    }

    // ------------------------------------------------------------
    // TraceReader: row shorter than header is skipped (sensor/transport/host)
    // ------------------------------------------------------------
    {
        fs::path trace_dir = tmp / "short_row_trace";
        fs::create_directory(trace_dir);

        // sensor.csv with a short row (only 1 field instead of 16)
        write_file(trace_dir / "sensor.csv",
            "seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us\n"
            "1,0,0.0,256,48000,1,0.0,0.0,120.0,4,4,100,1,1,0.0,50\n"
            "2\n"  // short row - should be skipped
            "3,0,0.01,256,48000,1,0.01,1.0,120.0,4,4,100,1,1,1.0,50\n"
        );
        write_file(trace_dir / "transport.csv", "t,pass,play_state,pos,pos2,cursor,rate,repeat,loop_start,loop_end,tempo,ts_num,ts_den,measure,beat,state_count\n");
        write_file(trace_dir / "host.csv", "t,dur_us,frames,srate,lost\n");
        write_file(trace_dir / "events.csv", "t,kind,detail\n");
        write_file(trace_dir / "meta.json", "{}");

        TraceReader reader = load_trace(trace_dir);
        // Only 2 valid rows (first and third), short row skipped
        CHECK(reader.sensor().size() == 2);
        CHECK_NEAR(reader.sensor()["seq"][0], 1.0, 1e-12);
        CHECK_NEAR(reader.sensor()["seq"][1], 3.0, 1e-12);
    }

    // ------------------------------------------------------------
    // CsvFollower: quoted cells (comma, doubled quote, newline in quotes)
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "quoted.csv";
        // detail column has: comma inside quotes, doubled quote, newline inside quotes
        write_file(csv, "t,kind,detail\n1.0,start,\"hello, world\"\n2.0,note,\"he said \"\"hi\"\"\"\n3.0,wrap,\"line1\nline2\"\n");

        CsvFollower follower(csv, EVENT_COLUMNS);
        auto rows = follower.read();

        CHECK(rows.size() == 3);
        CHECK(rows[0][2] == "hello, world");        // comma in quotes
        CHECK(rows[1][2] == "he said \"hi\"");      // doubled quotes
        CHECK(rows[2][2] == "line1\nline2");        // newline in quotes
    }

    // ------------------------------------------------------------
    // TraceReader: load_trace throws on missing sensor.csv
    // ------------------------------------------------------------
    {
        fs::path empty_dir = tmp / "no_sensor";
        fs::create_directory(empty_dir);
        write_file(empty_dir / "transport.csv", "t,pass\n1,2\n");

        bool threw = false;
        try {
            load_trace(empty_dir);
        } catch (const std::runtime_error &e) {
            threw = true;
            std::string msg = e.what();
            CHECK(msg.find("no sensor.csv") != std::string::npos);
        }
        CHECK(threw);
    }

    // ------------------------------------------------------------
    // TraceReader: basic load and update
    // ------------------------------------------------------------
    {
        fs::path trace_dir = tmp / "trace1";
        fs::create_directory(trace_dir);

        write_file(trace_dir / "sensor.csv",
            "seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us\n"
            "1,0,0.0,256,48000,1,0.0,0.0,120.0,4,4,100,1,1,0.0,50\n"
        );
        write_file(trace_dir / "transport.csv",
            "t,pass,play_state,pos,pos2,cursor,rate,repeat,loop_start,loop_end,tempo,ts_num,ts_den,measure,beat,state_count\n"
            "0.0,1,1,0.0,0.0,0.0,1.0,0,0.0,0.0,120.0,4,4,1,1,1\n"
        );
        write_file(trace_dir / "host.csv",
            "t,dur_us,frames,srate,lost\n"
            "0.0,5333,256,48000,0\n"
        );
        write_file(trace_dir / "events.csv",
            "t,kind,detail\n"
            "0.0,start,\"recording started\"\n"
        );
        write_file(trace_dir / "meta.json", "{\"version\": 1}");

        TraceReader reader = load_trace(trace_dir);

        CHECK(reader.sensor().size() == 1);
        CHECK(reader.transport().size() == 1);
        CHECK(reader.host().size() == 1);
        CHECK(reader.events().size() == 1);
        CHECK(reader.meta_json().find("version") != std::string::npos);

        const auto &sensor_t = reader.sensor()["t"];
        CHECK_NEAR(sensor_t[0], 0.0, 1e-12);

        const auto &event = reader.events()[0];
        CHECK_NEAR(event.t, 0.0, 1e-12);
        CHECK(event.kind == "start");
        CHECK(event.detail == "recording started");

        // update() again returns 0 new rows
        CHECK(reader.update() == 0);
    }

    // ------------------------------------------------------------
    // TraceReader: incremental update (live mode)
    // ------------------------------------------------------------
    {
        fs::path trace_dir = tmp / "trace2";
        fs::create_directory(trace_dir);

        write_file(trace_dir / "sensor.csv",
            "seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us\n"
            "1,0,0.0,256,48000,1,0.0,0.0,120.0,4,4,100,1,1,0.0,50\n"
        );
        write_file(trace_dir / "transport.csv", "t,pass,play_state,pos,pos2,cursor,rate,repeat,loop_start,loop_end,tempo,ts_num,ts_den,measure,beat,state_count\n");
        write_file(trace_dir / "host.csv", "t,dur_us,frames,srate,lost\n");
        write_file(trace_dir / "events.csv", "t,kind,detail\n");
        write_file(trace_dir / "meta.json", "{}");

        TraceReader reader = load_trace(trace_dir);
        CHECK(reader.sensor().size() == 1);

        // Append more sensor rows
        append_file(trace_dir / "sensor.csv",
            "2,0,0.005,256,48000,1,0.005,0.5,120.0,4,4,100,1,1,0.5,50\n"
            "3,0,0.010,256,48000,1,0.010,1.0,120.0,4,4,100,1,1,1.0,50\n"
        );

        CHECK(reader.update() == 2);
        CHECK(reader.sensor().size() == 3);
    }

    // ------------------------------------------------------------
    // TraceReader: events with missing detail (empty string)
    // ------------------------------------------------------------
    {
        fs::path trace_dir = tmp / "trace3";
        fs::create_directory(trace_dir);

        write_file(trace_dir / "sensor.csv",
            "seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us\n"
            "1,0,0.0,256,48000,1,0.0,0.0,120.0,4,4,100,1,1,0.0,50\n"
        );
        write_file(trace_dir / "events.csv",
            "t,kind,detail\n"
            "1.0,note,\n"       // empty detail
            "2.0,wrap\n"        // missing detail column entirely
        );
        write_file(trace_dir / "transport.csv", "t,pass,play_state,pos,pos2,cursor,rate,repeat,loop_start,loop_end,tempo,ts_num,ts_den,measure,beat,state_count\n");
        write_file(trace_dir / "host.csv", "t,dur_us,frames,srate,lost\n");
        write_file(trace_dir / "meta.json", "{}");

        TraceReader reader = load_trace(trace_dir);
        CHECK(reader.events().size() == 2);
        CHECK(reader.events()[0].detail == "");
        CHECK(reader.events()[1].detail == "");
    }

    // ------------------------------------------------------------
    // TraceReader: NaN for empty/unreadable cells in tables
    // ------------------------------------------------------------
    {
        fs::path trace_dir = tmp / "trace4";
        fs::create_directory(trace_dir);

        write_file(trace_dir / "sensor.csv",
            "seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us\n"
            "1,0,,256,48000,1,0.0,0.0,120.0,4,4,100,1,1,0.0,50\n"   // empty t -> NaN
            "2,0,0.005,abc,48000,1,0.005,0.5,120.0,4,4,100,1,1,0.5,50\n"  // unreadable frames -> NaN
        );
        write_file(trace_dir / "transport.csv", "t,pass,play_state,pos,pos2,cursor,rate,repeat,loop_start,loop_end,tempo,ts_num,ts_den,measure,beat,state_count\n");
        write_file(trace_dir / "host.csv", "t,dur_us,frames,srate,lost\n");
        write_file(trace_dir / "events.csv", "t,kind,detail\n");
        write_file(trace_dir / "meta.json", "{}");

        TraceReader reader = load_trace(trace_dir);
        const auto &t_col = reader.sensor()["t"];
        const auto &frames_col = reader.sensor()["frames"];

        CHECK(std::isnan(t_col[0]));
        CHECK(std::isnan(frames_col[1]));
        CHECK_NEAR(t_col[1], 0.005, 1e-9);
        CHECK_NEAR(frames_col[0], 256.0, 1e-9);
    }

    // ------------------------------------------------------------
    // A row cut off inside a quoted cell is not lost: it waits until the writer finishes it
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "cut_in_quotes.csv";
        write_file(csv, "t,kind,detail\n1,a,\"first line\nsecond");
        CsvFollower follower(csv, EVENT_COLUMNS);
        CHECK(follower.read().empty());
        append_file(csv, " line\"\n");
        auto rows = follower.read();
        CHECK(rows.size() == 1);
        CHECK(!rows.empty() && rows[0][2] == "first line\nsecond line");
    }

    // ------------------------------------------------------------
    // As in Python's csv module, a quote only opens a quoted cell at the start of the cell
    // ------------------------------------------------------------
    {
        fs::path csv = tmp / "quote_inside.csv";
        write_file(csv, "t,kind,detail\n1,a,say \"hi\" now\n");
        CsvFollower follower(csv, EVENT_COLUMNS);
        auto rows = follower.read();
        CHECK(rows.size() == 1);
        CHECK(!rows.empty() && rows[0][2] == "say \"hi\" now");
    }

    // ------------------------------------------------------------
    // Out-of-range numbers keep their value, like Python's float(); they are not "unreadable"
    // ------------------------------------------------------------
    CHECK(std::isinf(parse_number("1e400")));

    // ------------------------------------------------------------
    // Cleanup
    // ------------------------------------------------------------
    remove_dir(tmp);

    return finish_tests();
}