// Tests for the HUD engine.

#include "hud_engine.h"
#include "simulate.h"

#include <cstdio>
#include "test_support.h"
#include "trace.h"

#include <filesystem>
#include <memory>
#include <string>

namespace sessionscope {

// Helper: create a ReplaySensor with a high speed so every row is due immediately.
std::unique_ptr<ReplaySensor> make_replay_sensor(const std::filesystem::path &folder) {
    // Use speed=1e9 like the Python test: (clock() - start) * 1e9 puts us far past the trace end.
    auto clock = []() { return now_seconds(); };
    return std::make_unique<ReplaySensor>(folder, 1e9, clock);
}

std::unique_ptr<ReplayHost> make_replay_host(ReplaySensor &sensor) {
    return std::make_unique<ReplayHost>(sensor);
}

HudEngine make_engine(const std::filesystem::path &folder) {
    auto sensor = make_replay_sensor(folder);
    ReplaySensor *sensor_ptr = sensor.get();
    auto host = std::make_unique<ReplayHost>(*sensor_ptr);
    return HudEngine(std::move(sensor), std::move(host), "test");
}

// The staircase climbs until its blocks overrun: they are counted, each once, and the most recent
// are listed with how late they were and where.
void test_overruns_are_counted_once() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_overruns";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();
    const Snapshot s = e.snapshot(0.0);
    const double overrun_tier = metric_by_key("lateness").tiers[2];
    CHECK(s.overruns > 0);
    CHECK(!s.recent_overruns.empty());
    CHECK(s.recent_overruns.size() <= RECENT_OVERRUNS);
    CHECK(s.recent_overruns.size() <= s.overruns);
    for (const Overrun &o : s.recent_overruns) {
        CHECK(o.late >= overrun_tier);
        CHECK(o.ago_s >= 0);
        CHECK(o.bar.has_value());
    }
    for (std::size_t i = 1; i < s.recent_overruns.size(); ++i) {
        CHECK(s.recent_overruns[i].ago_s <= s.recent_overruns[i - 1].ago_s);   // newest last
    }

    // No new blocks: the count stays.
    e.tick();
    CHECK(e.snapshot(0.0).overruns == s.overruns);

    std::filesystem::remove_all(tmp);
}

void test_snapshot_has_everything_the_page_draws() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_staircase";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();
    Snapshot s = e.snapshot(0.0);

    CHECK(s.ready);
    CHECK(s.meter.size() == static_cast<size_t>(METER_COLS));
    CHECK(s.cpu_meter.has_value());
    CHECK(s.cpu_meter->size() == static_cast<size_t>(METER_COLS));
    CHECK(s.cpu.has_value());
    CHECK(s.cpu->p99.has_value());
    CHECK(!s.bars.empty());
    CHECK(s.chart_metrics.size() == 2);
    CHECK(s.chart_metrics[0]->key == "lateness");
    CHECK(s.chart_metrics[1]->key == "host_rt");

    std::filesystem::remove_all(tmp);
}

void test_cpu_per_bar_rises_with_the_staircase() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_staircase2";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();
    e.set_metric("host_rt");
    Snapshot s = e.snapshot(0.0);

    CHECK(s.metric->key == "host_rt");
    const auto &row = s.bars[0];
    std::vector<double> values;
    for (const auto &kv : row.cells) {
        if (kv.second.value.has_value()) {
            values.push_back(*kv.second.value);
        }
    }
    CHECK(values.size() > 4);
    CHECK(values.back() > values.front());

    // Check host blocks land on sensor's bars
    size_t placed = 0;
    for (int pid : e.host_bars().pass_id()) {
        if (pid) ++placed;
    }
    double ratio = e.host_bars().size() > 0 ? static_cast<double>(placed) / e.host_bars().size() : 0.0;
    CHECK(ratio > 0.9);

    std::filesystem::remove_all(tmp);
}

void test_spectrogram_columns_cover_the_window_once() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_staircase3";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();
    auto cols1 = e.snapshot(0.0).spec_cols;

    std::vector<int> ids1;
    for (const auto &c : cols1) ids1.push_back(c.id);
    CHECK(ids1.size() > 100);
    CHECK(std::is_sorted(ids1.begin(), ids1.end()));
    // Each column appears once
    CHECK(ids1.end() == std::unique(ids1.begin(), ids1.end()));
    for (const auto &c : cols1) {
        CHECK(c.bins.empty() || c.bins.size() == static_cast<size_t>(SPEC_BINS));
    }

    // Second snapshot: nothing new
    auto cols2 = e.snapshot(0.0).spec_cols;
    CHECK(cols2.empty());

    // spec_pos checks
    CHECK(std::abs(spec_pos(SPEC_LO) - 0.0) < 1e-9);
    CHECK(std::abs(spec_pos(SPEC_HI) - 1.0) < 1e-9);

    std::filesystem::remove_all(tmp);
}

void test_unknown_metric_is_ignored() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_staircase4";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.set_metric("nonsense");
    CHECK(e.metric()->key == "lateness");

    std::filesystem::remove_all(tmp);
}

void test_live_sensor_waiting_on_private_ring() {
    // LiveSensor on a private ring name that does not exist should be "waiting"
    LiveSensor sensor("SessionScope_test_private_ring_that_does_not_exist_12345");
    CHECK(sensor.status() == "waiting");

    LiveHost host("SessionScope_test_private_ring_that_does_not_exist_12345");
    CHECK(host.status() == "waiting");

    HudEngine engine(std::make_unique<LiveSensor>("SessionScope_test_private_ring_12345"),
                     std::make_unique<LiveHost>("SessionScope_test_private_ring_12345"),
                     "live", std::nullopt);
    auto snap = engine.snapshot(0.0);
    CHECK(snap.status == "waiting");
    CHECK(!snap.ready);
}

void test_replay_finished_when_clock_past_last_row() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_finished";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "song", "song");

    double clock_time = 0.0;
    auto clock = [&clock_time]() { return clock_time; };
    ReplaySensor sensor(folder, 1.0, clock);
    ReplayHost host(sensor);

    // Initially not finished
    CHECK(!sensor.finished());
    CHECK(!host.finished());

    // Advance clock far past the trace
    clock_time = 1e9;
    sensor.poll();
    host.poll();
    CHECK(sensor.finished());
    CHECK(host.finished());

    std::filesystem::remove_all(tmp);
}

void test_nan_values_reach_snapshot_as_empty_optionals() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_nan";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Use a trace with some NaN values (e.g., tempo might be NaN in stopped sections)
    auto folder = write_simulated_trace(tmp / "song", "song");

    HudEngine e = make_engine(folder);
    e.tick();
    Snapshot s = e.snapshot(0.0);

    // Check that NaN values become empty optionals, not numbers
    // In a playing section, tempo should have a value
    if (s.playing && s.tempo.has_value()) {
        CHECK(!std::isnan(*s.tempo));
    }

    // Check meter values: NaN -> empty optional
    for (const auto &v : s.meter) {
        if (v.has_value()) {
            CHECK(!std::isnan(*v));
        }
    }

    if (s.cpu_meter.has_value()) {
        for (const auto &v : *s.cpu_meter) {
            if (v.has_value()) {
                CHECK(!std::isnan(*v));
            }
        }
    }

    std::filesystem::remove_all(tmp);
}

void test_bar_chart_throttling() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_throttle";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();

    // First snapshot computes bars
    auto s1 = e.snapshot(0.0);
    auto bars1 = s1.bars;

    // Second snapshot at same time returns cached bars
    auto s2 = e.snapshot(0.0);
    CHECK(s2.bars.size() == bars1.size());

    // Third snapshot after BARS_EVERY_S recomputes
    auto s3 = e.snapshot(BARS_EVERY_S + 0.001);
    // Should be the same data since nothing changed, but it's a recomputation
    CHECK(s3.bars.size() == bars1.size());

    std::filesystem::remove_all(tmp);
}

void test_host_rt_metric_switch() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_switch";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "staircase", "staircase");

    HudEngine e = make_engine(folder);
    e.tick();

    // Default metric is lateness
    CHECK(e.metric()->key == "lateness");
    auto s1 = e.snapshot(0.0);
    CHECK(s1.metric->key == "lateness");

    // Switch to host_rt
    e.set_metric("host_rt");
    CHECK(e.metric()->key == "host_rt");
    auto s2 = e.snapshot(0.0);
    CHECK(s2.metric->key == "host_rt");

    // Switch back
    e.set_metric("lateness");
    CHECK(e.metric()->key == "lateness");

    std::filesystem::remove_all(tmp);
}

void test_snapshot_metric_structure() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_metric_struct";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    auto folder = write_simulated_trace(tmp / "song", "song");

    HudEngine e = make_engine(folder);
    e.tick();
    Snapshot s = e.snapshot(0.0);

    CHECK(s.metric != nullptr);
    CHECK(!s.metric->key.empty());
    CHECK(!s.metric->title.empty());
    CHECK(!s.metric->unit.empty());
    CHECK(s.metric->tiers.size() == 3);
    CHECK(s.metric->scale > 0);

    std::filesystem::remove_all(tmp);
}

void test_lost_count_includes_sensor_dropped() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_lost";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Create a trace with drop_every=97
    SimulateOptions opts;
    opts.drop_every = 97;
    auto folder = write_simulated_trace(tmp / "song_drop", "song", opts);

    HudEngine e = make_engine(folder);
    e.tick();
    Snapshot s = e.snapshot(0.0);

    // lost should include both missing from blocks and dropped from sensor
    CHECK(s.lost > 0);

    std::filesystem::remove_all(tmp);
}

// Dropping old rows must not change anything the HUD shows: two engines replay the same song on
// the same stepped clock, one keeping only 13 s of rows (the least allowed: the meter shows 12 s),
// so it drops rows over and over, the other
// all of them. Every snapshot must match, and the first engine's table must stay small.
void test_dropping_old_rows_changes_nothing_shown() {
    auto tmp = std::filesystem::temp_directory_path() / "sessionscope_test_hud_history";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    auto folder = write_simulated_trace(tmp / "song", "song");

    double clock_now = 0.0;   // both replays read this clock; the test moves it on
    const auto make = [&](double history_s) {
        auto sensor = std::make_unique<ReplaySensor>(folder, 1.0, [&clock_now] { return clock_now; });
        ReplaySensor *sensor_ptr = sensor.get();
        HudEngine engine(std::move(sensor), std::make_unique<ReplayHost>(*sensor_ptr), "test");
        engine.set_history_seconds(history_s);
        return engine;
    };
    HudEngine trimmed = make(13.0);
    HudEngine full = make(1e9);

    const auto same_bars = [](const std::vector<BarRow> &a, const std::vector<BarRow> &b) {
        if (a.size() != b.size()) return false;
        for (std::size_t r = 0; r < a.size(); ++r) {
            if (a[r].label != b[r].label || a[r].pass != b[r].pass || a[r].cells.size() != b[r].cells.size()) return false;
            for (const auto &[bar, cell] : a[r].cells) {
                const auto other = b[r].cells.find(bar);
                if (other == b[r].cells.end() || other->second.n != cell.n || other->second.value != cell.value) return false;
            }
        }
        return true;
    };

    int steps = 0, mismatches = 0;
    std::size_t largest_table = 0;
    for (; steps < 6000 && !static_cast<const ReplaySensor &>(trimmed.source()).finished(); ++steps) {
        clock_now += 0.1;
        trimmed.tick();
        full.tick();
        const Snapshot a = trimmed.snapshot(clock_now);
        const Snapshot b = full.snapshot(clock_now);
        bool same = a.ready == b.ready && a.bar == b.bar && a.pass == b.pass && a.late.p99 == b.late.p99 &&
                    a.meter == b.meter && a.lost == b.lost && same_bars(a.bars, b.bars) &&
                    a.spec_cols.size() == b.spec_cols.size();
        for (std::size_t c = 0; same && c < a.spec_cols.size(); ++c) {
            same = a.spec_cols[c].id == b.spec_cols[c].id && a.spec_cols[c].bins == b.spec_cols[c].bins;
        }
        if (!same && mismatches++ == 0) {
            std::fprintf(stderr, "  first difference at step %d: ready %d bar %d pass %d p99 %d meter %d lost %d bars %d spec %d\n",
                         steps, a.ready == b.ready, a.bar == b.bar, a.pass == b.pass, a.late.p99 == b.late.p99,
                         a.meter == b.meter, a.lost == b.lost, same_bars(a.bars, b.bars),
                         a.spec_cols.size() == b.spec_cols.size());
        }
        largest_table = std::max(largest_table, trimmed.source().sensor().size());
    }
    CHECK(steps > 300);                      // the song is long enough to drop rows many times
    CHECK(mismatches == 0);
    CHECK(full.source().sensor().size() > 3 * largest_table);   // the full engine kept far more
    CHECK(largest_table < 48000 / 256 * 27);  // at most about twice the 13 s history, plus a tick

    std::filesystem::remove_all(tmp);
}

}  // namespace sessionscope

int main() {
    sessionscope::test_snapshot_has_everything_the_page_draws();
    sessionscope::test_cpu_per_bar_rises_with_the_staircase();
    sessionscope::test_spectrogram_columns_cover_the_window_once();
    sessionscope::test_unknown_metric_is_ignored();
    sessionscope::test_live_sensor_waiting_on_private_ring();
    sessionscope::test_replay_finished_when_clock_past_last_row();
    sessionscope::test_nan_values_reach_snapshot_as_empty_optionals();
    sessionscope::test_bar_chart_throttling();
    sessionscope::test_host_rt_metric_switch();
    sessionscope::test_snapshot_metric_structure();
    sessionscope::test_lost_count_includes_sensor_dropped();
    sessionscope::test_dropping_old_rows_changes_nothing_shown();
    sessionscope::test_overruns_are_counted_once();

    return finish_tests();
}
