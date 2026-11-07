#ifndef SESSIONSCOPE_REPORT_H
#define SESSIONSCOPE_REPORT_H

// The trace report: what was recorded, where in the song the pressure was, and the spike checks.
// Wording says what was observed, with its source and window, and never more. Correlation is
// reported as correlation.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "blocks.h"
#include "checks.h"
#include "json.h"
#include "render.h"
#include "stats.h"
#include "trace.h"

namespace sessionscope {

// One heatmap lane: per-pass cells, their median across passes, and the hottest stretch.
struct Lane {
    const Metric *metric = nullptr;
    PassCells per_pass;
    BarCells heat;
    std::optional<std::pair<int, int>> hot;
};

// Builds one lane from blocks, a metric, quantile q, and min_blocks.
Lane lane(const Blocks &blocks, const Metric &metric, double q, int min_blocks);
Lane lane(const HostBlocks &blocks, const Metric &metric, double q, int min_blocks);

// Everything the text and JSON reports show, computed once.
// Throws std::runtime_error if metric_key == "host_rt" and no host.csv exists.
struct Summary {
    Lane main;
    std::optional<Lane> rt;
    std::optional<HostBlocks> host;
    std::vector<Check> checks;
    std::vector<double> self_us;
    std::vector<double> xruns;
};

Summary summarize(const TraceReader &trace, const Blocks &blocks,
                  std::string_view metric_key = "lateness", int min_blocks = DEFAULT_MIN_BLOCKS, double q = DEFAULT_Q);

// (bar, pass) of the sensor block playing at time t, e.g. where an xrun happened.
std::optional<std::pair<int, int>> bar_of_time(const TraceReader &trace, const Blocks &blocks, double t);

// Title, heatmap rows (passes and their median) and legend for one lane.
std::vector<std::string> heat_section(const Style &style, const Lane &lane, double q = DEFAULT_Q, int max_pass_rows = 8);

// The full console report: header, passes, heatmaps, observations, spike checks.
std::string report_text(const TraceReader &trace,
                        std::string_view metric_key = "lateness",
                        int min_blocks = DEFAULT_MIN_BLOCKS,
                        double q = DEFAULT_Q,
                        const Style *style = nullptr,
                        int max_pass_rows = 8);

// One sentence about a lane's hottest stretch of bars (bar numbers only), or its highest bar.
std::optional<std::string> hot_line(const Lane &lane, double q);

// The "Observations" list: what was seen, where, and what is missing. Never a cause.
std::vector<std::string> findings(const TraceReader &trace, const Blocks &blocks, const Summary &s, double q);

// The same report as data (report --json): for scripts, and later for the HUD.
JsonValue report_json(const TraceReader &trace,
                      std::string_view metric_key = "lateness",
                      int min_blocks = DEFAULT_MIN_BLOCKS,
                      double q = DEFAULT_Q);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_REPORT_H
