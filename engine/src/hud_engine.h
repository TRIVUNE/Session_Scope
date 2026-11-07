#ifndef SESSIONSCOPE_HUD_ENGINE_H
#define SESSIONSCOPE_HUD_ENGINE_H

// HUD engine: turns live or replayed data into one snapshot per frame for the native window.
// There is no window and no server in this file. The window calls tick() and snapshot() from
// one thread; no locks are needed.
//
// It began as a port of the earlier Python engine and was checked against it frame by frame;
// engine/tests/test_hud_engine.cpp now holds that behaviour in place.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "blocks.h"
#include "json.h"
#include "ring_reader.h"
#include "stats.h"
#include "trace.h"

namespace sessionscope {

// The meter's time span, columns and spectrogram range.
constexpr double METER_S = 12.0;
constexpr int METER_COLS = 240;
constexpr double BARS_EVERY_S = 0.5;
constexpr int MAX_PASS_ROWS = 6;
constexpr double SPEC_COL_S = METER_S / METER_COLS;   // 0.05 s
constexpr int SPEC_BINS = 72;
constexpr double SPEC_LO = -0.5;
constexpr double SPEC_HI = 3.0;
constexpr double SPEC_K = 10.0;
// Sensor rows kept, in seconds: well beyond METER_S, and beyond how late host blocks arrive.
// Rows are dropped in batches, once twice this much has built up.
constexpr double HISTORY_S = 60.0;
// Host blocks kept after they were counted into the bar chart (about 100 s at 48 kHz / 256).
constexpr std::size_t HOST_ROWS_KEPT = 20000;
// A bar needs fewer blocks in the live chart than in a report: the chart fills in while a pass plays.
constexpr int BAR_CHART_MIN_BLOCKS = 20;

// spec_pos: lateness (share of a block) -> 0..1 on the spectrogram's vertical axis.
// engine/hud/hud_view.cpp draws its axis with this same formula.
inline double spec_pos(double v) {
    const double lo = std::asinh(SPEC_K * SPEC_LO);
    const double hi = std::asinh(SPEC_K * SPEC_HI);
    const double g = std::asinh(SPEC_K * v);
    double pos = (g - lo) / (hi - lo);
    if (pos < 0.0) pos = 0.0;
    if (pos > 1.0) pos = 1.0;
    return pos;
}

// Forward declare now_seconds from sensor_ring.h
double now_seconds();

// -----------------------------------------------------------------------------
// Abstract sources: the engine works the same on live and replay sources.

// Forward declaration
class Table;

class SensorSource {
public:
    virtual ~SensorSource() = default;
    virtual std::string_view status() = 0;
    virtual std::size_t poll() = 0;
    virtual bool second_sensor() = 0;
    virtual const Table &sensor() const = 0;
    virtual std::uint64_t dropped() const = 0;
    virtual double host_process_id() const = 0;
    // Forgets the oldest rows of sensor() (Table::drop_front), so a long session doesn't grow.
    virtual void drop_front(std::size_t rows) = 0;
};

class HostSource {
public:
    virtual ~HostSource() = default;
    virtual std::string_view status() = 0;
    virtual std::size_t poll() = 0;
    virtual const std::vector<SharedHost::HostBlock> &fresh() const = 0;
    virtual const std::deque<SharedHost::HostBlock> &blocks() const = 0;
    virtual std::vector<double> recent(double seconds = 1.0) const = 0;
};

// -----------------------------------------------------------------------------
// Live wrappers around SharedSensor and SharedHost.

class LiveSensor : public SensorSource {
public:
    explicit LiveSensor(const char *mapping_name = MAPPING_NAME);
    explicit LiveSensor(std::string mapping_name);  // for tests: private ring name
    std::string_view status() override;
    std::size_t poll() override;
    bool second_sensor() override;
    const Table &sensor() const override;
    std::uint64_t dropped() const override;
    double host_process_id() const override;
    void drop_front(std::size_t rows) override { sensor_.drop_front(rows); }

private:
    SharedSensor sensor_;
};

class LiveHost : public HostSource {
public:
    explicit LiveHost(const char *mapping_name = HOST_MAPPING_NAME);
    explicit LiveHost(std::string mapping_name);  // for tests: private ring name
    std::string_view status() override;
    std::size_t poll() override;
    const std::vector<SharedHost::HostBlock> &fresh() const override;
    const std::deque<SharedHost::HostBlock> &blocks() const override;
    std::vector<double> recent(double seconds = 1.0) const override;

private:
    SharedHost host_;
};

// -----------------------------------------------------------------------------
// Replay sources: play a trace folder back on a clock. The clock is a std::function<double()>
// defaulting to now_seconds (wall time). Tests pass a stepped clock for exact reproducibility.

class ReplaySensor : public SensorSource {
public:
    ReplaySensor(const std::filesystem::path &folder, double speed = 1.0,
                 std::function<double()> clock = now_seconds);
    std::string_view status() override;
    std::size_t poll() override;
    bool second_sensor() override;
    const Table &sensor() const override;
    std::uint64_t dropped() const override;
    double host_process_id() const override;
    void drop_front(std::size_t rows) override { sensor_.drop_front(rows); }

    double now() const;
    bool finished() const;
    double speed() const { return speed_; }
    const Table &source_table() const { return src_; }
    std::size_t source_index() const { return i_; }
    const TraceReader &trace_reader() const { return trace_; }

private:
    TraceReader trace_;
    double speed_;
    std::function<double()> clock_;
    const Table &src_;
    Table sensor_;
    std::uint64_t dropped_ = 0;
    double t0_ = 0.0;
    double start_ = 0.0;
    std::size_t i_ = 0;
};

class ReplayHost : public HostSource {
public:
    explicit ReplayHost(ReplaySensor &sensor);
    std::string_view status() override;
    std::size_t poll() override;
    const std::vector<SharedHost::HostBlock> &fresh() const override;
    const std::deque<SharedHost::HostBlock> &blocks() const override;
    std::vector<double> recent(double seconds = 1.0) const override;

    bool finished() const;
    double speed() const { return sensor_.speed(); }
    const Table &source_table() const { return src_; }
    std::size_t source_index() const { return i_; }

private:
    ReplaySensor &sensor_;
    const Table &src_;
    std::deque<SharedHost::HostBlock> blocks_;
    std::vector<SharedHost::HostBlock> fresh_;
    std::size_t i_ = 0;
    std::size_t lost_ = 0;
};

// -----------------------------------------------------------------------------
// HostOnBars: places REAPER's host blocks on the sensor's passes and bars by time.

class HostOnBars {
public:
    static constexpr double NEAR_S = 0.05;  // a host block more than this after the last sensor block is left unplaced

    HostOnBars() = default;

    void add(const std::vector<SharedHost::HostBlock> &fresh,
             const std::vector<double> &sensor_t,
             const Blocks &blocks);

    const std::vector<int> &pass_id() const { return pass_id_; }
    const std::vector<int> &bar() const { return bar_; }
    const std::vector<double> &load() const { return load_; }
    std::size_t size() const { return pass_id_.size(); }

    // Forgets the first `rows` host blocks (already counted into the bar chart).
    void drop_front(std::size_t rows);

private:
    std::vector<int> pass_id_;
    std::vector<int> bar_;
    std::vector<double> load_;
};

// -----------------------------------------------------------------------------
// Snapshot: everything the HUD shows, as plain C++ data (no JSON rounding).

struct CpuSnapshot {
    std::optional<double> avg;
    std::optional<double> p99;
    std::optional<double> max;
    std::optional<int> tier;
};

struct SpectrogramColumn {
    int id;
    std::vector<int> bins;  // empty for silence
};

struct BarCell {
    std::optional<double> value;
    int n = 0;
};

struct BarRow {
    std::string label;
    int pass;                          // 0 for Median row
    std::map<int, BarCell> cells;      // bar -> cell
};

// A block that ran at least the "overrun" tier late (lateness metric, tiers[2]): close enough to
// its deadline that the audio may have dropped out.
struct Overrun {
    double ago_s;               // seconds before the newest block
    std::optional<int> bar;     // the bar it fell in, if the host was playing
    double late;                // how late, as a share of the block
};
constexpr std::size_t RECENT_OVERRUNS = 8;   // how many the snapshot lists

struct PassWorst {
    std::optional<double> value;
    std::optional<int> bar;
    std::optional<int> tier;
};

struct Snapshot {
    std::string source;
    std::optional<std::string> note;
    std::string status;                // "waiting", "live", "stale"
    const Metric *metric = nullptr;
    double meter_s = METER_S;
    std::vector<const Metric *> chart_metrics;
    std::array<double, 3> late_tiers = {0.10, 0.20, 0.40};
    double late_scale = 0.45;

    std::optional<CpuSnapshot> cpu;
    std::optional<std::vector<std::optional<double>>> cpu_meter;

    bool ready = false;
    // When ready:
    bool playing = false;
    std::optional<int> bar;
    std::optional<double> tempo;
    std::array<std::optional<double>, 2> ts = {std::nullopt, std::nullopt};
    std::optional<double> srate;
    std::optional<double> frames;
    std::optional<double> deadline_ms;
    std::optional<int> pass;
    std::size_t lost = 0;
    bool second_sensor = false;
    struct { std::optional<double> p99; std::optional<int> tier; } late;
    std::vector<std::optional<double>> meter;
    std::vector<SpectrogramColumn> spec_cols;
    int spec_bins = SPEC_BINS;
    double spec_lo = SPEC_LO;
    double spec_hi = SPEC_HI;
    double spec_k = SPEC_K;
    double spec_col_s = SPEC_COL_S;
    std::vector<BarRow> bars;
    PassWorst pass_worst;

    // Overruns since the HUD started, and the most recent ones, newest last.
    std::size_t overruns = 0;
    std::vector<Overrun> recent_overruns;

    // The bar clicked in the list (select_bar), pass by pass; empty when none is selected.
    std::optional<int> selected_bar;
    std::vector<BarPassDetail> selected_detail;
};

// -----------------------------------------------------------------------------
// HudEngine: the main class. Constructed from unique_ptr<SensorSource> and unique_ptr<HostSource>.

class HudEngine {
public:
    HudEngine(std::unique_ptr<SensorSource> sensor,
              std::unique_ptr<HostSource> host,
              std::string label = "live",
              std::optional<std::string> note = std::nullopt);

    void set_metric(std::string_view key);
    void tick();
    Snapshot snapshot(double now);   // `now` replaces time.perf_counter for the bar chart's throttle

    // How many seconds of sensor rows the engine keeps (default HISTORY_S). Everything the
    // snapshot shows except the bar chart covers at most the last METER_S seconds, and the bar
    // chart keeps its own per-bar values, so older rows are dropped. Smaller values are for tests.
    // Never less than the meter's span plus a second: the meter reads that far back.
    // The bar whose blocks the next snapshots describe (selected_bar, selected_detail), or none.
    void select_bar(std::optional<int> bar) { selected_bar_ = bar; }

    void set_history_seconds(double seconds) { history_s_ = std::max(seconds, METER_S + 1.0); }

    // Test accessors
    const HostOnBars &host_bars() const { return host_bars_; }
    const SensorSource &source() const { return *src_; }
    const Metric *metric() const { return metric_; }

private:
    // Per-block values
    std::optional<double> late(std::size_t k) const;

    // Meters
    std::vector<std::optional<double>> meter(std::size_t i);
    std::optional<std::vector<std::optional<double>>> cpu_meter();

    // Spectrogram columns completed since last call
    std::vector<SpectrogramColumn> spec_columns(std::size_t n);

    // Bar chart (throttled)
    std::vector<BarRow> bar_chart(std::size_t i);

    // Counts the new blocks into the bar chart cells, then drops rows the snapshot no longer needs.
    void count_new_blocks();
    void forget_old_rows();

    std::unique_ptr<SensorSource> src_;
    std::unique_ptr<HostSource> host_;
    std::string label_;
    std::optional<std::string> note_;
    Blocks blocks_;
    HostOnBars host_bars_;
    const Metric *metric_;
    std::optional<std::vector<BarRow>> bars_;
    // The bar chart's cells for each metric it can show, kept up to date every tick, so the chart
    // never needs old blocks and switching metric is instant.
    LiveBarCells late_cells_;
    LiveBarCells host_cells_;
    bool host_seen_ = false;                 // any host block arrived (the chart can show host_rt)
    std::optional<int> selected_bar_;        // the bar clicked in the HUD's list
    double history_s_ = HISTORY_S;           // seconds of sensor rows kept
    double bars_at_ = 0.0;
    std::size_t spec_from_ = 0;              // first sensor row not yet put in a spectrogram column
    std::size_t overrun_from_ = 0;           // first sensor row not yet checked for an overrun
    std::size_t overruns_ = 0;
    struct OverrunAt {
        double t;
        std::optional<int> bar;
        double late;
    };
    std::deque<OverrunAt> recent_overruns_;
    std::map<int, std::vector<int>> spec_open_;  // column id -> bin counts
    std::vector<std::optional<double>> meter_buf_;   // reused between calls
    std::vector<std::optional<double>> cpu_meter_buf_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_ENGINE_H
