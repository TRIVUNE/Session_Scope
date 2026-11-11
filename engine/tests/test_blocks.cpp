// Tests for blocks.h: turning sensor rows into blocks with a bar, a pass and a lateness, across
// gaps, seeks, loops and tempo changes.

#include "blocks.h"
#include "trace.h"
#include "test_support.h"
#include <cmath>
#include <vector>

using namespace sessionscope;

// ---------------------------------------------------------------------
// Helpers: a sensor table built row by row
// ---------------------------------------------------------------------

constexpr double D = 256.0 / 48000.0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Default sensor row at 48 kHz / 256 block at 120 bpm.
static Table make_sensor_table(const std::vector<std::vector<double>> &rows) {
    Table t(SENSOR_COLUMNS);
    for (const auto &row : rows) {
        t.append_row(row);
    }
    return t;
}

// Base row with all defaults, matching helpers.py sensor_table().
static std::vector<double> base_row(std::size_t i, double t0 = 100.0, double pos0 = 0.0, int state = 1) {
    const double d = D;
    const double t = t0 + i * d;
    const double play_pos = pos0 + i * d;
    const double qn = play_pos * 2.0;  // 120 bpm = 2 beats per second
    return {
        static_cast<double>(i),        // seq
        1.0,                           // instance
        t,                             // t
        256.0,                         // frames
        48000.0,                       // srate
        static_cast<double>(state),    // play_state
        play_pos,                      // play_pos
        qn,                            // qn
        120.0,                         // tempo
        4.0,                           // ts_num
        4.0,                           // ts_den
        1.0,                           // self_us
        kNaN,                          // r_measure
        kNaN,                          // r_beat
        kNaN,                          // r_qn
        kNaN                           // workload_us
    };
}

// steady(n, start_seq=0, t0=100.0, pos0=0.0, state=1, **extra)
// Returns n consecutive rows with the given parameters.
static std::vector<std::vector<double>> steady(int n, int start_seq = 0, double t0 = 100.0,
                                               double pos0 = 0.0, int state = 1) {
    std::vector<std::vector<double>> rows;
    rows.reserve(n);
    for (int k = 0; k < n; ++k) {
        auto row = base_row(static_cast<std::size_t>(start_seq + k), t0, pos0, state);
        row[0] = static_cast<double>(start_seq + k); // seq
        row[2] = t0 + k * D;                         // t
        row[6] = pos0 + k * D;                       // play_pos
        row[7] = (pos0 + k * D) * 2.0;               // qn
        row[5] = static_cast<double>(state);         // play_state
        rows.push_back(std::move(row));
    }
    return rows;
}

// Overload to allow modifying specific columns after creation (for tests like test_a_late_callback).
static std::vector<std::vector<double>> steady_modifiable(int n, int start_seq = 0, double t0 = 100.0,
                                                          double pos0 = 0.0, int state = 1) {
    return steady(n, start_seq, t0, pos0, state);
}

// Concatenate two row vectors.
static std::vector<std::vector<double>> operator+(const std::vector<std::vector<double>> &a,
                                                  const std::vector<std::vector<double>> &b) {
    std::vector<std::vector<double>> c;
    c.reserve(a.size() + b.size());
    c.insert(c.end(), a.begin(), a.end());
    c.insert(c.end(), b.begin(), b.end());
    return c;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

// Test: Lateness.on_time_callbacks_have_zero_lateness
static void test_on_time_callbacks_have_zero_lateness() {
    auto rows = steady(10);
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK(std::isnan(b.lateness_ms()[0]));
    for (std::size_t i = 1; i < b.lateness_ms().size(); ++i) {
        CHECK_NEAR(b.lateness_ms()[i], 0.0, 1e-9);
    }
    CHECK_NEAR(b.d_ms()[0], 5.3333333, 1e-6);
}

// Test: Lateness.a_late_callback
static void test_a_late_callback() {
    auto rows = steady_modifiable(5);
    // For rows[3:] add 0.002 to t
    for (std::size_t i = 3; i < rows.size(); ++i) {
        rows[i][2] += 0.002;  // t column
    }
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK_NEAR(b.lateness_ms()[3], 2.0, 1e-6);
    CHECK_NEAR(b.lateness_ms()[4], 0.0, 1e-6);
}

// Test: Lateness.no_lateness_across_lost_records
static void test_no_lateness_across_lost_records() {
    auto rows = steady(6);
    rows.erase(rows.begin() + 3);  // the recorder never saw seq 3
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK(b.missing() == 1);
    CHECK(std::isnan(b.lateness_ms()[3]));
    CHECK(b.passes().size() == 1);  // lost records do not split the pass
}

// Test: Lateness.sensor_restart
static void test_sensor_restart() {
    auto part1 = steady(4);
    // Second part starts at seq=0, t0=100+4*D, pos0=4*D
    auto part2 = steady(4, 0, 100.0 + 4 * D, 4 * D);
    auto rows = part1 + part2;
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK(b.resets() == 1);
    CHECK(std::isnan(b.lateness_ms()[4]));
}

// Test: Passes.stop_and_start
static void test_stop_and_start() {
    auto part1 = steady(5);
    auto part2 = steady(5, 5, 100.0 + 5 * D, 5 * D, 0);  // state=0 (stopped)
    auto part3 = steady(5, 10, 100.0 + 10 * D, 5 * D);
    auto rows = part1 + part2 + part3;
    Blocks b = build_blocks(make_sensor_table(rows));

    std::vector<std::string> kinds;
    for (const auto &p : b.passes()) kinds.push_back(p.kind);
    CHECK(kinds.size() == 2);
    CHECK(kinds[0] == "start");
    CHECK(kinds[1] == "start");

    std::vector<int> expected_pass_id = {1,1,1,1,1, 0,0,0,0,0, 2,2,2,2,2};
    CHECK(b.pass_id() == expected_pass_id);
    CHECK(b.passes()[0].first == 0);
    CHECK(b.passes()[0].last == 4);
}

// Test: Passes.loop_wrap_is_a_new_pass
static void test_loop_wrap_is_a_new_pass() {
    auto part1 = steady(5, 0, 100.0, 10.0);  // pos0=10.0
    auto part2 = steady(5, 5, 100.0 + 5 * D, 8.0);  // pos0=8.0 (jump back)
    auto rows = part1 + part2;
    Blocks b = build_blocks(make_sensor_table(rows));

    std::vector<std::string> kinds;
    for (const auto &p : b.passes()) kinds.push_back(p.kind);
    CHECK(kinds.size() == 2);
    CHECK(kinds[0] == "start");
    CHECK(kinds[1] == "jump_back");
}

// Test: Passes.seek_forward_is_a_new_pass
static void test_seek_forward_is_a_new_pass() {
    auto part1 = steady(5);
    auto part2 = steady(5, 5, 100.0 + 5 * D, 30.0);  // pos0=30.0 (seek forward)
    auto rows = part1 + part2;
    Blocks b = build_blocks(make_sensor_table(rows));

    std::vector<std::string> kinds;
    for (const auto &p : b.passes()) kinds.push_back(p.kind);
    CHECK(kinds.size() == 2);
    CHECK(kinds[0] == "start");
    CHECK(kinds[1] == "jump");
}

// Test: Passes.fast_play_rate_is_not_a_seek
static void test_fast_play_rate_is_not_a_seek() {
    auto rows = steady_modifiable(10);
    // Multiply play_pos by 4 (4x speed)
    for (auto &row : rows) {
        row[6] *= 4.0;  // play_pos
        row[7] *= 4.0;  // qn
    }
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK(b.passes().size() == 1);
}

// Test: Bars.reaper_bar_wins_when_present
static void test_reaper_bar_wins_when_present() {
    std::vector<std::vector<double>> rows = { base_row(0) };
    rows[0][12] = 32.0;  // r_measure = 32
    rows[0][13] = kNaN;  // r_beat
    rows[0][14] = kNaN;  // r_qn
    rows[0][15] = kNaN;  // workload_us
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK(b.bar()[0] == 33);
}

// Test: Bars.naive_bar_from_quarter_notes
static void test_naive_bar_from_quarter_notes() {
    CHECK(naive_bar(0, 4, 4) == 1);
    CHECK(naive_bar(15.99, 4, 4) == 4);
    CHECK(naive_bar(16, 4, 4) == 5);
    CHECK(naive_bar(6, 3, 4) == 3);
    CHECK(naive_bar(7, 7, 8) == 3);
    CHECK(naive_bar(kNaN, 4, 4) == 0);
}

// Test: Bars.workload_share
static void test_workload_share() {
    std::vector<std::vector<double>> rows = { base_row(0) };
    rows[0][15] = 2666.6666667;  // workload_us
    Blocks b = build_blocks(make_sensor_table(rows));

    CHECK_NEAR(b.workload_share()[0], 0.5, 1e-6);
}

// Test: Incremental.rows_arriving_in_pieces_give_the_same_result
static void test_rows_arriving_in_pieces_give_the_same_result() {
    auto part1 = steady(5);
    auto part2 = steady(5, 5, 100.0 + 5 * D, 1.0);
    auto all_rows = part1 + part2;

    // Build all at once
    Blocks whole = build_blocks(make_sensor_table(all_rows));

    // Build incrementally
    auto growing_rows = std::vector<std::vector<double>>(all_rows.begin(), all_rows.begin() + 3);
    Table growing = make_sensor_table(growing_rows);
    Blocks part(growing);
    part.update();

    for (std::size_t i = 3; i < all_rows.size(); ++i) {
        growing.append_row(all_rows[i]);
        part.update();
    }
    part.finish();

    CHECK(part.pass_id() == whole.pass_id());

    std::vector<std::string> part_kinds, whole_kinds;
    for (const auto &p : part.passes()) part_kinds.push_back(p.kind);
    for (const auto &p : whole.passes()) whole_kinds.push_back(p.kind);
    CHECK(part_kinds == whole_kinds);

    // Compare lateness from index 1 onwards (index 0 is always NaN)
    for (std::size_t i = 1; i < part.lateness_ms().size(); ++i) {
        CHECK_NEAR(part.lateness_ms()[i], whole.lateness_ms()[i], 1e-9);
    }
}

// ---------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------

int main() {
    test_on_time_callbacks_have_zero_lateness();
    test_a_late_callback();
    test_no_lateness_across_lost_records();
    test_sensor_restart();
    test_stop_and_start();
    test_loop_wrap_is_a_new_pass();
    test_seek_forward_is_a_new_pass();
    test_fast_play_rate_is_not_a_seek();
    test_reaper_bar_wins_when_present();
    test_naive_bar_from_quarter_notes();
    test_workload_share();
    test_rows_arriving_in_pieces_give_the_same_result();

    return finish_tests();
}
