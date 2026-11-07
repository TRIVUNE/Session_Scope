// The HUD engine; hud_engine.h describes what a snapshot holds.

#include "hud_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace sessionscope {

namespace {

// Floor division the way the original engine defined it (Python's float //). It goes through
// fmod, so it is exact: 5000 // 0.05 is 99999, because 0.05 as a double is slightly more than
// 0.05. floor(5000 / 0.05) would round the division first and give 100000.
double floor_divide(double dividend, double divisor) {
    double remainder = std::fmod(dividend, divisor);
    double quotient = (dividend - remainder) / divisor;
    if (remainder != 0 && ((divisor < 0) != (remainder < 0))) quotient -= 1.0;
    if (quotient == 0) return std::copysign(0.0, dividend / divisor);
    double floored = std::floor(quotient);
    if (quotient - floored > 0.5) floored += 1.0;
    return floored;
}

// The spectrogram column a time falls in: columns are numbered from the clock's zero, so the page
// can place a column exactly whatever its frame rate.
int spectrogram_column(double t) { return static_cast<int>(floor_divide(t, SPEC_COL_S)); }

// Copies rows from one table to another with the same columns. Column references are taken once.
class RowCopier {
public:
    RowCopier(const Table &from, const std::vector<std::string> &columns) : row_(columns.size()) {
        for (const std::string &column : columns) columns_.push_back(&from[column]);
    }
    void copy(std::size_t row, Table &to) {
        for (std::size_t c = 0; c < columns_.size(); ++c) row_[c] = (*columns_[c])[row];
        to.append_row(row_);
    }

private:
    std::vector<const std::vector<double> *> columns_;
    std::vector<double> row_;
};

}  // namespace

// -----------------------------------------------------------------------------
// LiveSensor

LiveSensor::LiveSensor(const char *mapping_name) : sensor_(mapping_name) {}
LiveSensor::LiveSensor(std::string mapping_name) : sensor_(mapping_name.c_str()) {}

std::string_view LiveSensor::status() { return sensor_.status(); }
std::size_t LiveSensor::poll() { return sensor_.poll(); }
bool LiveSensor::second_sensor() { return sensor_.second_sensor(); }
const Table &LiveSensor::sensor() const { return sensor_.sensor(); }
std::uint64_t LiveSensor::dropped() const { return sensor_.dropped(); }
double LiveSensor::host_process_id() const { return sensor_.host_process_id(); }

// -----------------------------------------------------------------------------
// LiveHost

LiveHost::LiveHost(const char *mapping_name) : host_(mapping_name) {}
LiveHost::LiveHost(std::string mapping_name) : host_(mapping_name.c_str()) {}

std::string_view LiveHost::status() { return host_.status(); }
std::size_t LiveHost::poll() { return host_.poll(); }
const std::vector<SharedHost::HostBlock> &LiveHost::fresh() const { return host_.fresh(); }
const std::deque<SharedHost::HostBlock> &LiveHost::blocks() const { return host_.blocks(); }
std::vector<double> LiveHost::recent(double seconds) const { return host_.recent(seconds); }

// -----------------------------------------------------------------------------
// ReplaySensor

ReplaySensor::ReplaySensor(const std::filesystem::path &folder, double speed,
                           std::function<double()> clock)
    : trace_(folder)
    , speed_(speed)
    , clock_(std::move(clock))
    , src_(trace_.sensor())
    , sensor_(SENSOR_COLUMNS)
    , start_(clock_()) {
    trace_.update();  // Load all trace data
    if (src_.size() > 0) {
        t0_ = src_["t"][0];
    }
}

std::string_view ReplaySensor::status() {
    if (finished()) return "stale";
    return "live";
}

std::size_t ReplaySensor::poll() {
    // Hand out every row whose time has come.
    const double t_now = now();
    const auto &t = src_["t"];
    RowCopier copier(src_, SENSOR_COLUMNS);
    std::size_t got = 0;
    for (; i_ < src_.size() && t[i_] <= t_now; ++i_, ++got) copier.copy(i_, sensor_);
    return got;
}

bool ReplaySensor::second_sensor() { return false; }

const Table &ReplaySensor::sensor() const { return sensor_; }
std::uint64_t ReplaySensor::dropped() const { return dropped_; }
double ReplaySensor::host_process_id() const { return 0.0; }

double ReplaySensor::now() const {
    return t0_ + (clock_() - start_) * speed_;
}

bool ReplaySensor::finished() const {
    return i_ >= src_.size();
}

// -----------------------------------------------------------------------------
// ReplayHost

ReplayHost::ReplayHost(ReplaySensor &sensor) : sensor_(sensor), src_(sensor.trace_reader().host()) {}

std::string_view ReplayHost::status() {
    if (src_.size() == 0) return "waiting";
    if (sensor_.finished()) return "stale";
    return "live";
}

std::size_t ReplayHost::poll() {
    // The recorder's sensor and host.csv share one clock, so there is no offset to apply here.
    const double t_now = sensor_.now();
    const auto &t = src_["t"];
    const auto &frames = src_["frames"];
    const auto &srate = src_["srate"];
    const auto &dur_us = src_["dur_us"];
    fresh_.clear();
    for (; i_ < src_.size() && t[i_] <= t_now; ++i_) {
        if (frames[i_] > 0 && srate[i_] > 0) {   // load = time taken / deadline
            fresh_.push_back({t[i_], dur_us[i_] / (frames[i_] / srate[i_] * 1e6)});
        }
    }
    blocks_.insert(blocks_.end(), fresh_.begin(), fresh_.end());
    return fresh_.size();
}

const std::vector<SharedHost::HostBlock> &ReplayHost::fresh() const { return fresh_; }
const std::deque<SharedHost::HostBlock> &ReplayHost::blocks() const { return blocks_; }

std::vector<double> ReplayHost::recent(double seconds) const {
    if (blocks_.empty()) return {};
    double t_end = blocks_.back().t;
    std::vector<double> result;
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
        if (t_end - it->t > seconds) break;
        result.push_back(it->load);
    }
    return result;
}

bool ReplayHost::finished() const {
    (void)lost_;  // tracked but not exposed, like Python
    return i_ >= src_.size();
}

// -----------------------------------------------------------------------------
// HostOnBars

void HostOnBars::drop_front(std::size_t rows) {
    rows = std::min(rows, pass_id_.size());
    pass_id_.erase(pass_id_.begin(), pass_id_.begin() + static_cast<std::ptrdiff_t>(rows));
    bar_.erase(bar_.begin(), bar_.begin() + static_cast<std::ptrdiff_t>(rows));
    load_.erase(load_.begin(), load_.begin() + static_cast<std::ptrdiff_t>(rows));
}

void HostOnBars::add(const std::vector<SharedHost::HostBlock> &fresh,
                     const std::vector<double> &sensor_t,
                     const Blocks &blocks) {
    std::size_t n = blocks.size();  // sensor rows that already have a pass and bar
    const auto &blocks_pass_id = blocks.pass_id();
    const auto &blocks_bar = blocks.bar();

    for (const auto &hb : fresh) {
        // Python: bisect.bisect_right(sensor_t, t, 0, n) - 1
        auto it = std::upper_bound(sensor_t.begin(), sensor_t.begin() + n, hb.t);
        std::ptrdiff_t k = (it == sensor_t.begin()) ? -1 : static_cast<std::ptrdiff_t>(it - sensor_t.begin() - 1);

        bool placed = (k >= 0) && (hb.t - sensor_t[k] < NEAR_S);
        pass_id_.push_back(placed ? blocks_pass_id[k] : 0);
        bar_.push_back(placed ? blocks_bar[k] : 0);
        load_.push_back(hb.load);
    }
}

// Helper: metric values of the host's blocks placed on bars, from row `first` on (host_rt only;
// any other metric has no host values).
namespace {
std::vector<double> host_on_bars_metric_values(const HostOnBars &hb, const Metric &metric, std::size_t first) {
    const std::size_t n = hb.size();
    if (first > n) first = n;
    if (metric.key != "host_rt") return std::vector<double>(n - first, std::numeric_limits<double>::quiet_NaN());
    const auto &load = hb.load();
    return std::vector<double>(load.begin() + static_cast<std::ptrdiff_t>(first), load.end());
}
}  // namespace

// -----------------------------------------------------------------------------
// HudEngine

HudEngine::HudEngine(std::unique_ptr<SensorSource> sensor,
                     std::unique_ptr<HostSource> host,
                     std::string label,
                     std::optional<std::string> note)
    : src_(std::move(sensor))
    , host_(std::move(host))
    , label_(std::move(label))
    , note_(std::move(note))
    , blocks_(src_->sensor())
    , host_bars_()
    , metric_(&metric_by_key("lateness"))
    , late_cells_(DEFAULT_Q, BAR_CHART_MIN_BLOCKS)
    , host_cells_(DEFAULT_Q, BAR_CHART_MIN_BLOCKS)
    , bars_at_(0.0) {
    meter_buf_.resize(METER_COLS);
    cpu_meter_buf_.resize(METER_COLS);
}

void HudEngine::set_metric(std::string_view key) {
    static const std::array<std::string_view, 2> CHART_METRICS = {"lateness", "host_rt"};
    bool known = false;
    for (auto m : CHART_METRICS) {
        if (m == key) {
            known = true;
            break;
        }
    }
    if (known && key != metric_->key) {
        metric_ = &metric_by_key(key);
        bars_.reset();
    }
}

void HudEngine::tick() {
    src_->poll();
    host_->poll();
    blocks_.update();
    if (!host_->fresh().empty()) {
        const auto &sensor_t = src_->sensor()["t"];
        host_bars_.add(host_->fresh(), sensor_t, blocks_);
        host_seen_ = true;
    }
    count_new_blocks();
    forget_old_rows();
}

void HudEngine::count_new_blocks() {
    late_cells_.add(blocks_.pass_id(), blocks_.bar(),
                    metric_values(blocks_, metric_by_key("lateness"), late_cells_.done()));
    host_cells_.add(host_bars_.pass_id(), host_bars_.bar(),
                    host_on_bars_metric_values(host_bars_, metric_by_key("host_rt"), host_cells_.done()));

    // Overruns: each block checked once, as it arrives.
    const double overrun = metric_by_key("lateness").tiers[2];
    const auto &t = src_->sensor()["t"];
    const std::size_t n = std::min(t.size(), blocks_.size());
    for (; overrun_from_ < n; ++overrun_from_) {
        const std::optional<double> v = late(overrun_from_);
        if (!v || *v < overrun) continue;
        ++overruns_;
        const int bar = blocks_.bar()[overrun_from_];
        recent_overruns_.push_back({t[overrun_from_], bar ? std::optional<int>(bar) : std::nullopt, *v});
        if (recent_overruns_.size() > RECENT_OVERRUNS) recent_overruns_.pop_front();
    }
}

void HudEngine::forget_old_rows() {
    // Sensor rows: once the oldest is twice the history old, drop everything older than the history.
    const auto &t = src_->sensor()["t"];
    const std::size_t n = std::min(t.size(), blocks_.size());
    if (n > 1 && t[n - 1] - t[0] > 2 * history_s_) {
        std::size_t rows = static_cast<std::size_t>(std::lower_bound(t.begin(), t.begin() + n, t[n - 1] - history_s_) - t.begin());
        rows = std::min(rows, n - 1);   // keep at least the newest row: the next block is compared with it
        src_->drop_front(rows);
        blocks_.drop_front(rows);
        late_cells_.forget_rows(rows);
        spec_from_ = spec_from_ > rows ? spec_from_ - rows : 0;
        overrun_from_ = overrun_from_ > rows ? overrun_from_ - rows : 0;
    }
    // Host blocks: once counted, only a short tail is kept.
    if (host_bars_.size() > 2 * HOST_ROWS_KEPT) {
        const std::size_t rows = host_bars_.size() - HOST_ROWS_KEPT;
        host_bars_.drop_front(rows);
        host_cells_.forget_rows(rows);
    }
}

std::optional<double> HudEngine::late(std::size_t k) const {
    const auto &B = blocks_;
    if (!B.pass_id()[k] || B.d_ms()[k] <= 0 || std::isnan(B.lateness_ms()[k])) {
        return std::nullopt;
    }
    return B.lateness_ms()[k] / B.d_ms()[k];
}

std::vector<std::optional<double>> HudEngine::meter(std::size_t i) {
    const auto &t = src_->sensor()["t"];
    double now_t = t[i];

    // Find the start of the window using lower_bound
    std::size_t start = std::lower_bound(t.begin(), t.end(), now_t - METER_S) - t.begin();

    // Initialize buffer with nullopt
    std::fill(meter_buf_.begin(), meter_buf_.end(), std::nullopt);

    for (std::size_t k = start; k <= i; ++k) {
        auto v = late(k);
        if (!v.has_value()) continue;

        int col = METER_COLS - 1 - static_cast<int>((now_t - t[k]) / METER_S * METER_COLS);
        if (col < 0 || col >= METER_COLS) continue;

        if (!meter_buf_[col].has_value() || *v > *meter_buf_[col]) {
            meter_buf_[col] = v;
        }
    }
    return meter_buf_;
}

std::optional<std::vector<std::optional<double>>> HudEngine::cpu_meter() {
    const auto &blocks = host_->blocks();
    if (blocks.empty()) return std::nullopt;

    double now_t = blocks.back().t;

    std::fill(cpu_meter_buf_.begin(), cpu_meter_buf_.end(), std::nullopt);

    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        double age = now_t - it->t;
        if (age > METER_S) break;

        int col = METER_COLS - 1 - static_cast<int>(age / METER_S * METER_COLS);
        if (col < 0 || col >= METER_COLS) continue;

        if (!cpu_meter_buf_[col].has_value() || it->load > *cpu_meter_buf_[col]) {
            cpu_meter_buf_[col] = it->load;
        }
    }
    return cpu_meter_buf_;
}

std::vector<SpectrogramColumn> HudEngine::spec_columns(std::size_t n) {
    const auto &t = src_->sensor()["t"];
    std::size_t start = spec_from_;
    if (spec_from_ == 0 && n > 0) {
        start = std::lower_bound(t.begin(), t.end(), t[n - 1] - METER_S) - t.begin();
    }

    for (std::size_t k = start; k < n; ++k) {
        int cid = spectrogram_column(t[k]);
        auto &col = spec_open_[cid];
        if (col.empty()) col.resize(SPEC_BINS, 0);

        auto v = late(k);
        if (v.has_value()) {
            int bin = static_cast<int>(spec_pos(*v) * SPEC_BINS);
            if (bin >= SPEC_BINS) bin = SPEC_BINS - 1;
            col[bin] += 1;
        }
    }
    spec_from_ = n;

    std::vector<SpectrogramColumn> out;
    if (n == 0) return out;

    int newest = spectrogram_column(t[n - 1]);   // still filling: keep it open
    for (auto it = spec_open_.begin(); it != spec_open_.end();) {
        if (it->first < newest) {
            SpectrogramColumn sc;
            sc.id = it->first;
            bool any = false;
            for (int count : it->second) {
                if (count > 0) { any = true; break; }
            }
            if (any) sc.bins = std::move(it->second);
            out.push_back(std::move(sc));
            it = spec_open_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

namespace {

BarRow bar_row(std::string label, int pass, const BarCells &cells) {
    BarRow row;
    row.label = std::move(label);
    row.pass = pass;
    for (const auto &[bar, cell] : cells) row.cells[bar] = BarCell{cell.value, cell.n};
    return row;
}

// One row per recent pass (at most MAX_PASS_ROWS, newest last) and, with two or more passes, the
// median across them, which only means something once there is more than one.
std::vector<BarRow> bar_rows(const PassCells &per_pass) {
    std::vector<BarRow> rows;
    const std::size_t skipped = per_pass.size() > static_cast<std::size_t>(MAX_PASS_ROWS)
                                    ? per_pass.size() - MAX_PASS_ROWS : 0;
    std::size_t index = 0;
    for (const auto &[pass, cells] : per_pass) {   // a map: passes come out in order
        if (index++ >= skipped) rows.push_back(bar_row("Pass " + std::to_string(pass), pass, cells));
    }
    if (per_pass.size() > 1) rows.push_back(bar_row("Median", 0, across_passes(per_pass)));
    return rows;
}

}  // namespace

std::vector<BarRow> HudEngine::bar_chart(std::size_t) {
    // The cells are counted every tick (count_new_blocks); this only turns them into rows.
    return bar_rows(metric_->key == "host_rt" ? host_cells_.cells() : late_cells_.cells());
}

Snapshot HudEngine::snapshot(double now) {
    Snapshot out;
    out.source = label_;
    out.note = note_;
    out.status = src_->status();
    out.metric = metric_;
    out.meter_s = METER_S;

    // chart_metrics: host_rt only when host_bars has rows
    static const std::array<std::string_view, 2> CHART_METRICS = {"lateness", "host_rt"};
    for (auto key : CHART_METRICS) {
        if (key != "host_rt" || host_seen_) {
            out.chart_metrics.push_back(&metric_by_key(key));
        }
    }
    out.late_tiers = metric_by_key("lateness").tiers;
    out.late_scale = metric_by_key("lateness").scale;

    const auto &S = src_->sensor();
    const auto &B = blocks_;
    std::size_t n = std::min(S.size(), B.size());

    bool host_live = host_->status() != "waiting";
    std::vector<double> loads = host_live ? host_->recent(1.0) : std::vector<double>{};

    if (!loads.empty()) {
        CpuSnapshot cpu;
        double sum = 0.0;
        for (double v : loads) sum += v;
        cpu.avg = sum / loads.size();
        cpu.p99 = quantile(loads, 0.99);
        cpu.max = *std::max_element(loads.begin(), loads.end());
        cpu.tier = tier(metric_by_key("host_rt"), *cpu.p99);
        out.cpu = cpu;
    }

    out.cpu_meter = host_live ? cpu_meter() : std::nullopt;

    if (out.status == "waiting" || n == 0) {
        out.ready = false;
        return out;
    }

    std::size_t i = n - 1;
    const auto &t = S["t"];

    // recent lateness for p99 (last 1 second)
    std::size_t recent_start = std::lower_bound(t.begin(), t.end(), t[i] - 1.0) - t.begin();
    std::vector<double> recent;
    for (std::size_t k = recent_start; k < n; ++k) {
        auto v = late(k);
        if (v.has_value()) recent.push_back(*v);
    }
    double late_p99 = std::numeric_limits<double>::quiet_NaN();
    if (!recent.empty()) late_p99 = quantile(recent, 0.99);

    int pid = B.pass_id()[i];
    out.ready = true;
    out.playing = (out.status == "live" && is_playing(S["play_state"][i]));
    out.bar = (B.bar()[i] != 0) ? std::optional<int>(B.bar()[i]) : std::nullopt;
    out.tempo = std::isnan(S["tempo"][i]) ? std::nullopt : std::optional<double>(S["tempo"][i]);
    out.ts[0] = std::isnan(S["ts_num"][i]) ? std::nullopt : std::optional<double>(S["ts_num"][i]);
    out.ts[1] = std::isnan(S["ts_den"][i]) ? std::nullopt : std::optional<double>(S["ts_den"][i]);
    out.srate = std::isnan(S["srate"][i]) ? std::nullopt : std::optional<double>(S["srate"][i]);
    out.frames = std::isnan(S["frames"][i]) ? std::nullopt : std::optional<double>(S["frames"][i]);
    out.deadline_ms = std::isnan(B.d_ms()[i]) ? std::nullopt : std::optional<double>(B.d_ms()[i]);
    out.pass = (pid != 0) ? std::optional<int>(pid) : std::nullopt;
    out.lost = B.missing() + src_->dropped();
    out.second_sensor = src_->second_sensor();
    out.late.p99 = std::isnan(late_p99) ? std::nullopt : std::optional<double>(late_p99);
    out.late.tier = std::isnan(late_p99) ? std::nullopt : std::optional<int>(tier(metric_by_key("lateness"), late_p99));

    out.meter = meter(i);
    out.overruns = overruns_;
    for (const OverrunAt &o : recent_overruns_) out.recent_overruns.push_back({t[i] - o.t, o.bar, o.late});
    auto spec_cols = spec_columns(n);
    out.spec_cols = std::move(spec_cols);
    out.spec_bins = SPEC_BINS;
    out.spec_lo = SPEC_LO;
    out.spec_hi = SPEC_HI;
    out.spec_k = SPEC_K;
    out.spec_col_s = SPEC_COL_S;

    // Bar chart throttled
    if (!bars_.has_value() || now - bars_at_ >= BARS_EVERY_S) {
        bars_ = bar_chart(i);
        bars_at_ = now;
    }
    out.bars = *bars_;

    PassWorst pw;
    if (pid != 0) {
        for (const auto &row : out.bars) {
            if (row.pass == pid) {
                std::vector<std::pair<double, int>> vals;
                for (const auto &cell_kv : row.cells) {
                    if (cell_kv.second.value.has_value()) {
                        vals.emplace_back(*cell_kv.second.value, cell_kv.first);
                    }
                }
                if (!vals.empty()) {
                    auto worst = *std::max_element(vals.begin(), vals.end(),
                                                   [](const auto &a, const auto &b) { return a.first < b.first; });
                    pw.value = worst.first;
                    pw.bar = worst.second;
                    pw.tier = tier(*metric_, worst.first);
                }
                break;
            }
        }
    }
    out.pass_worst = pw;

    if (selected_bar_) {
        const LiveBarCells &cells = metric_->key == "host_rt" ? host_cells_ : late_cells_;
        out.selected_bar = selected_bar_;
        out.selected_detail = cells.bar_detail(*selected_bar_, metric_->tiers[2]);
    }
    return out;
}

}  // namespace sessionscope
