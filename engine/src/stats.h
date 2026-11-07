#ifndef SESSIONSCOPE_STATS_H
#define SESSIONSCOPE_STATS_H

// Per-bar statistics: each bar of each pass gets an upper percentile, then bars are combined
// across passes with the median of those values, never a pooled percentile. Adjacent blocks are
// correlated, so passes are the independent unit. Too few blocks gives no number at all.

#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "blocks.h"

namespace sessionscope {

constexpr int DEFAULT_MIN_BLOCKS = 50;
constexpr double DEFAULT_Q = 0.99;

// Nearest-rank quantile: the smallest value with at least q of the data at or below it.
// Uses std::nth_element for O(n) selection. Empty input gives NaN.
double quantile(std::vector<double> values, double q);

// Median: NaN for empty; even count averages the two middle values.
double median(std::vector<double> values);

// A heatmap metric: how to extract one value per block, and how to judge and colour it.
struct Metric {
    std::string key;
    std::string title;
    std::string unit;
    std::array<double, 3> tiers;   // thresholds for elevated, at risk, overrun
    double scale;                  // value at which the colour scale is fully red
};

// Returns the metric with the given key. Throws std::out_of_range if unknown.
const Metric &metric_by_key(std::string_view key);

// Extract metric values from Blocks (lateness, workload, lateness_ms, interval).
// Returns NaN where the value is unknown (e.g., division by zero).
// `first`: start at that row, for live data that only needs the rows added since last time.
std::vector<double> metric_values(const Blocks &blocks, const Metric &metric, std::size_t first = 0);

// Extract metric values from HostBlocks (host_rt only).
// Returns NaN where the value is unknown.
std::vector<double> metric_values(const HostBlocks &blocks, const Metric &metric);

// Tier classification: 0 normal, 1 elevated, 2 at risk, 3 overrun.
// Returns -1 for NaN (Python's None).
int tier(const Metric &metric, double v);

constexpr const char *TIER_NAMES[4] = {"normal", "elevated", "at risk", "overrun"};

// One heatmap cell: a bar of a pass, or a bar across passes.
struct Cell {
    std::optional<double> value;   // empty when too few blocks for a percentile
    int n = 0;                     // blocks seen in the bar
    int passes = 1;                // passes that contributed a value (across-pass cells only)

    // State string: "ok" if value present, "few" if n > 0 but no value, "none" if n == 0.
    const char *state() const;
};

using BarCells = std::map<int, Cell>;          // bar -> cell
using PassCells = std::map<int, BarCells>;     // pass_id -> bar cells

// Groups blocks by (pass, bar) and computes per-pass, per-bar percentile cells.
// Skips blocks where pass_id <= 0 or bar <= 0.
// NaN values count as seen but contribute no number.
// value is set only when at least min_blocks non-NaN values exist.
// One pass over blocks, grouping into a map keyed by (pass, bar) for speed.
PassCells bar_cells(const std::vector<int> &pass_ids,
                    const std::vector<int> &bars,
                    const std::vector<double> &values,
                    double q = DEFAULT_Q,
                    int min_blocks = DEFAULT_MIN_BLOCKS);

// Combines per-pass cells into across-pass cells: median of per-pass values that exist.
// n sums all blocks seen; passes counts how many passes contributed a value.
// value is empty when no pass contributed a value.
BarCells across_passes(const PassCells &cells_by_pass);

// One bar in one pass, from its blocks: what the HUD shows when a bar is clicked.
struct BarPassDetail {
    int pass = 0;
    int blocks = 0;                  // blocks seen in the bar
    std::optional<double> median;    // of the blocks with a value; empty if none has one
    std::optional<double> p99;       // the chart's percentile, the same as the bar's cell
    std::optional<double> worst;     // the single worst block
    int overruns = 0;                // blocks at or above the metric's overrun line
};

// The same cells as bar_cells(), for live data, kept up to date as blocks arrive. Live blocks are
// only ever appended, never changed, so each update reads just the new blocks and recomputes the
// percentile only of the bars they fell into. bar_cells() over a long session instead rereads
// every block since play started, every time.
class LiveBarCells {
public:
    LiveBarCells(double q, int min_blocks) : q_(q), min_blocks_(min_blocks) {}

    // Blocks read so far: the next add() starts at this row.
    std::size_t done() const { return done_; }

    // The first `rows` rows were dropped from the columns (they were read already): the next
    // add() starts that much earlier. The cells are unaffected.
    void forget_rows(std::size_t rows) { done_ = rows < done_ ? done_ - rows : 0; }

    // Adds the blocks from row done() on: `values` holds just those rows' values, while pass_ids
    // and bars are the full columns. If the columns got shorter (the data started over), starts
    // over too.
    void add(const std::vector<int> &pass_ids, const std::vector<int> &bars, const std::vector<double> &values);

    // The cells of every (pass, bar) so far, identical to bar_cells() over all the blocks.
    const PassCells &cells();

    // One bar's blocks, pass by pass (oldest pass first). `overrun_at`: the metric's overrun line.
    std::vector<BarPassDetail> bar_detail(int bar, double overrun_at) const;

private:
    struct Group {
        std::vector<double> known_values;
        int blocks_seen = 0;
        bool changed = true;   // has new blocks since its cell was last computed
    };
    double q_;
    int min_blocks_;
    std::size_t done_ = 0;
    std::map<std::pair<int, int>, Group> groups_;   // (pass, bar) -> its blocks
    PassCells cells_;
};

// Finds the longest run of consecutive bars at or above min_tier (default 2 = at risk).
// Returns (first_bar, last_bar) of the first longest run, or empty if none.
std::optional<std::pair<int, int>> hottest(const BarCells &heat,
                                           const Metric &metric,
                                           int min_tier = 2);

// Spearman rank correlation with average ranks for ties.
// Pairs with NaN on either side are dropped.
// Fewer than 3 pairs -> NaN.
// NaN when either side is constant (zero variance).
// Stable tie handling so identical inputs produce identical ranks.
double spearman(const std::vector<double> &xs, const std::vector<double> &ys);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_STATS_H
