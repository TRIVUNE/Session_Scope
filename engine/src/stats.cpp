// Per-bar statistics; stats.h explains the method.

#include "stats.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace sessionscope {

double quantile(std::vector<double> values, double q) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Nearest rank: k = min(n-1, max(0, ceil(q*n) - 1))
    std::size_t n = values.size();
    double qn = q * static_cast<double>(n);
    long k = static_cast<long>(std::ceil(qn)) - 1;
    if (k < 0) k = 0;
    if (k >= static_cast<long>(n)) k = static_cast<long>(n) - 1;
    std::nth_element(values.begin(), values.begin() + k, values.end());
    return values[k];
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::size_t n = values.size();
    if (n % 2 == 1) {
        std::size_t m = n / 2;
        std::nth_element(values.begin(), values.begin() + m, values.end());
        return values[m];
    } else {
        std::size_t m2 = n / 2;
        // Find both middle elements: nth_element for m2,
        // then find max of first m2 elements for m1 = m2 - 1.
        std::nth_element(values.begin(), values.begin() + m2, values.end());
        double v2 = values[m2];
        double v1 = *std::max_element(values.begin(), values.begin() + m2);
        return (v1 + v2) / 2.0;
    }
}

namespace {

// The five metrics a report or the HUD can rank bars by.
const Metric METRICS_DATA[] = {
    {"lateness",
     "Late callbacks (sensor estimate)",
     "share of a block",
     {0.10, 0.20, 0.40},
     0.45},
    {"workload",
     "Injected workload (known)",
     "share of the deadline",
     {0.70, 0.85, 1.0},
     1.05},
    {"host_rt",
     "Realtime CPU (host audio thread)",
     "share of the deadline",
     {0.70, 0.85, 1.0},
     1.05},
    {"lateness_ms",
     "Late callbacks",
     "ms",
     {0.5, 1.0, 2.0},
     2.4},
    {"interval",
     "Callback interval",
     "blocks",
     {1.10, 1.20, 1.40},
     1.5},
};

}  // namespace

const Metric &metric_by_key(std::string_view key) {
    for (const auto &m : METRICS_DATA) {
        if (m.key == key) {
            return m;
        }
    }
    throw std::out_of_range(std::string(key));
}

std::vector<double> metric_values(const Blocks &blocks, const Metric &metric, std::size_t first) {
    const std::size_t n = blocks.size();
    if (first > n) first = n;
    std::vector<double> result;
    result.reserve(n - first);
    // A column from row `first` to the end.
    const auto rows_from_first = [first](const std::vector<double> &column) {
        return std::vector<double>(column.begin() + static_cast<std::ptrdiff_t>(first), column.end());
    };

    if (metric.key == "lateness") {
        const auto &lateness = blocks.lateness_ms();
        const auto &d_ms = blocks.d_ms();
        for (std::size_t i = first; i < n; ++i) {
            if (d_ms[i] > 0) {
                result.push_back(lateness[i] / d_ms[i]);
            } else {
                result.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
    } else if (metric.key == "workload") {
        result = rows_from_first(blocks.workload_share());
    } else if (metric.key == "lateness_ms") {
        result = rows_from_first(blocks.lateness_ms());
    } else if (metric.key == "interval") {
        const auto &interval = blocks.interval_ms();
        const auto &d_ms = blocks.d_ms();
        for (std::size_t i = first; i < n; ++i) {
            if (d_ms[i] > 0) {
                result.push_back(interval[i] / d_ms[i]);
            } else {
                result.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
    } else {
        throw std::out_of_range(metric.key);
    }
    return result;
}

std::vector<double> metric_values(const HostBlocks &blocks, const Metric &metric) {
    std::size_t n = blocks.size();
    std::vector<double> result;
    result.reserve(n);

    if (metric.key == "host_rt") {
        result = blocks.load();
    } else {
        throw std::out_of_range(metric.key);
    }
    return result;
}

int tier(const Metric &metric, double v) {
    if (std::isnan(v)) {
        return -1;
    }
    int count = 0;
    for (double th : metric.tiers) {
        if (v >= th) {
            ++count;
        }
    }
    return count;
}

const char *Cell::state() const {
    if (value.has_value()) {
        return "ok";
    }
    return n > 0 ? "few" : "none";
}

PassCells bar_cells(const std::vector<int> &pass_ids,
                    const std::vector<int> &bars,
                    const std::vector<double> &values,
                    double q,
                    int min_blocks) {
    struct BarGroup {
        std::vector<double> known_values;
        int blocks_seen = 0;
    };
    std::map<std::pair<int, int>, BarGroup> groups;   // (pass, bar) -> its blocks

    // A bar lasts hundreds of blocks in a row, so look the group up only when the bar changes.
    BarGroup *current = nullptr;
    int current_pass = 0;
    int current_bar = 0;

    const std::size_t n = pass_ids.size();
    for (std::size_t i = 0; i < n; ++i) {
        const int pid = pass_ids[i];
        const int bar = bars[i];
        if (pid <= 0 || bar <= 0) continue;   // stopped, or not placed in a bar

        if (!current || pid != current_pass || bar != current_bar) {
            current = &groups[{pid, bar}];
            current_pass = pid;
            current_bar = bar;
        }
        ++current->blocks_seen;
        if (!std::isnan(values[i])) current->known_values.push_back(values[i]);   // NaN: seen, but no number
    }

    PassCells out;
    for (auto &[pass_and_bar, group] : groups) {
        const auto [pid, bar] = pass_and_bar;
        std::optional<double> value;
        if (group.known_values.size() >= static_cast<std::size_t>(min_blocks)) {
            value = quantile(std::move(group.known_values), q);   // the group is finished: no copy needed
        }
        out[pid][bar] = Cell{value, group.blocks_seen, 1};
    }
    return out;
}

void LiveBarCells::add(const std::vector<int> &pass_ids, const std::vector<int> &bars,
                       const std::vector<double> &values) {
    if (pass_ids.size() < done_) {   // the data started over
        groups_.clear();
        cells_.clear();
        done_ = 0;
    }
    Group *current = nullptr;
    int current_pass = 0;
    int current_bar = 0;
    for (std::size_t j = 0; j < values.size() && done_ + j < pass_ids.size(); ++j) {
        const int pid = pass_ids[done_ + j];
        const int bar = bars[done_ + j];
        if (pid <= 0 || bar <= 0) continue;   // stopped, or not placed in a bar: as in bar_cells()

        if (!current || pid != current_pass || bar != current_bar) {
            current = &groups_[{pid, bar}];
            current->changed = true;
            current_pass = pid;
            current_bar = bar;
        }
        ++current->blocks_seen;
        if (!std::isnan(values[j])) current->known_values.push_back(values[j]);
    }
    done_ += values.size();
    if (done_ > pass_ids.size()) done_ = pass_ids.size();
}

const PassCells &LiveBarCells::cells() {
    for (auto &[pass_and_bar, group] : groups_) {
        if (!group.changed) continue;   // nothing new in this bar: its cell stands
        const auto [pid, bar] = pass_and_bar;
        std::optional<double> value;
        if (group.known_values.size() >= static_cast<std::size_t>(min_blocks_)) {
            value = quantile(group.known_values, q_);   // a copy: the group keeps growing
        }
        cells_[pid][bar] = Cell{value, group.blocks_seen, 1};
        group.changed = false;
    }
    return cells_;
}

std::vector<BarPassDetail> LiveBarCells::bar_detail(int bar, double overrun_at) const {
    std::vector<BarPassDetail> out;
    for (const auto &[pass_and_bar, group] : groups_) {
        if (pass_and_bar.second != bar) continue;
        BarPassDetail d;
        d.pass = pass_and_bar.first;
        d.blocks = group.blocks_seen;
        if (!group.known_values.empty()) {
            d.median = median(group.known_values);
            d.worst = *std::max_element(group.known_values.begin(), group.known_values.end());
            if (group.known_values.size() >= static_cast<std::size_t>(min_blocks_)) d.p99 = quantile(group.known_values, q_);
            d.overruns = static_cast<int>(std::count_if(group.known_values.begin(), group.known_values.end(),
                                                        [overrun_at](double v) { return v >= overrun_at; }));
        }
        out.push_back(d);
    }
    return out;
}

BarCells across_passes(const PassCells &cells_by_pass) {
    std::map<int, std::vector<double>> per_bar_values;
    std::map<int, int> n_bar;
    std::map<int, int> pass_count;

    for (const auto &pass_kv : cells_by_pass) {
        const BarCells &cells = pass_kv.second;
        for (const auto &cell_kv : cells) {
            int bar = cell_kv.first;
            const Cell &cell = cell_kv.second;
            n_bar[bar] += cell.n;
            if (cell.value.has_value()) {
                per_bar_values[bar].push_back(cell.value.value());
                pass_count[bar] += 1;
            }
        }
    }

    BarCells result;
    for (const auto &kv : n_bar) {
        int bar = kv.first;
        int total_n = kv.second;
        int passes = pass_count[bar];
        std::optional<double> value;
        auto it = per_bar_values.find(bar);
        if (it != per_bar_values.end() && !it->second.empty()) {
            value = median(it->second);
        }
        result[bar] = Cell{value, total_n, passes};
    }
    return result;
}

std::optional<std::pair<int, int>> hottest(const BarCells &heat,
                                           const Metric &metric,
                                           int min_tier) {
    std::optional<std::pair<int, int>> best;
    std::optional<std::pair<int, int>> run;

    for (const auto &kv : heat) {
        int bar = kv.first;
        const Cell &cell = kv.second;
        int t = tier(metric, cell.value.value_or(std::numeric_limits<double>::quiet_NaN()));
        if (t >= min_tier) {
            if (run.has_value() && run->second == bar - 1) {
                run->second = bar;  // extend
            } else {
                run = std::make_pair(bar, bar);  // start new
            }
            if (!best.has_value() || (run->second - run->first > best->second - best->first)) {
                best = run;
            }
        } else {
            run.reset();
        }
    }
    return best;
}

double spearman(const std::vector<double> &xs, const std::vector<double> &ys) {
    // Drop pairs with NaN on either side.
    std::vector<std::pair<double, double>> pairs;
    std::size_t n = std::min(xs.size(), ys.size());
    pairs.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isnan(xs[i]) && !std::isnan(ys[i])) {
            pairs.emplace_back(xs[i], ys[i]);
        }
    }

    if (pairs.size() < 3) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Extract values for ranking.
    std::vector<double> x_vals, y_vals;
    x_vals.reserve(pairs.size());
    y_vals.reserve(pairs.size());
    for (const auto &p : pairs) {
        x_vals.push_back(p.first);
        y_vals.push_back(p.second);
    }

    auto ranks = [](const std::vector<double> &v) -> std::vector<double> {
        std::size_t m = v.size();
        std::vector<std::size_t> order(m);
        for (std::size_t i = 0; i < m; ++i) order[i] = i;
        // Stable sort by value to ensure consistent tie handling.
        std::stable_sort(order.begin(), order.end(),
                         [&v](std::size_t a, std::size_t b) { return v[a] < v[b]; });

        std::vector<double> r(m);
        std::size_t i = 0;
        while (i < m) {
            std::size_t j = i;
            while (j + 1 < m && v[order[j + 1]] == v[order[i]]) {
                ++j;
            }
            // Average rank for ties: (i + j) / 2 + 1 (1-based).
            double avg_rank = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
            for (std::size_t k = i; k <= j; ++k) {
                r[order[k]] = avg_rank;
            }
            i = j + 1;
        }
        return r;
    };

    std::vector<double> rx = ranks(x_vals);
    std::vector<double> ry = ranks(y_vals);

    double mx = 0.0, my = 0.0;
    for (double v : rx) mx += v;
    for (double v : ry) my += v;
    mx /= rx.size();
    my /= ry.size();

    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (std::size_t i = 0; i < rx.size(); ++i) {
        double dx = rx[i] - mx;
        double dy = ry[i] - my;
        sxy += dx * dy;
        sxx += dx * dx;
        syy += dy * dy;
    }

    if (sxx == 0.0 || syy == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return sxy / std::sqrt(sxx * syy);
}

}  // namespace sessionscope
