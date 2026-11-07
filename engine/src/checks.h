#ifndef SESSIONSCOPE_CHECKS_H
#define SESSIONSCOPE_CHECKS_H

// Spike checks from blueprint 10.4, computed from one trace.
//
// S1: Does the sensor's musical position map to the right bar? Compares the bar the sensor
//     implies (quarter notes and the current signature) with the host's own bar for the same
//     project time, and the sensor's position with the host's at the same moment.
// S2: Does the lateness proxy follow real deadline pressure? Compares it with the known
//     Workload cost per block (fixture F7) and with the host's own xruns.
//
// Each check reports evidence and a suggested reading; the decision goes in the research record.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blocks.h"
#include "json.h"
#include "stats.h"
#include "trace.h"

namespace sessionscope {

// The outcome of one spike check, as shown in the report and stored in its JSON.
struct Check {
    std::string name;
    std::string status;                    // "pass", "fail", "inconclusive", "no data"
    std::vector<std::string> lines;        // human-readable evidence, one sentence per line
    JsonValue data;                        // the same numbers, machine-readable (JSON object)

    Check() = default;
    // A check with no numbers, such as "no data", still has a data object: an empty one.
    Check(std::string n, std::string s, std::vector<std::string> l,
          JsonValue d = JsonValue(std::vector<std::pair<std::string, JsonValue>>{}))
        : name(std::move(n)), status(std::move(s)), lines(std::move(l)), data(std::move(d)) {}
};

// Bar line tolerance: a block this close to a bar line may be counted in either bar.
constexpr double BAR_LINE_QN = 1e-6;

// The note about REAPER's dummy audio device.
extern const std::string DUMMY_NOTE;

// S1: compare the sensor's bar with the host's bar for every playing block.
Check s1_bar_mapping(const TraceReader &trace, const Blocks &blocks);

// S2 against the host's own realtime load (host.csv), bar by bar, medians across passes.
// late_heat and rt_heat are bar -> Cell maps (across-pass cells from stats.h).
Check s2_vs_realtime(const TraceReader &trace,
                     const std::map<int, Cell> &late_heat,
                     const std::map<int, Cell> &rt_heat);

// S2 against the known Workload cost (fixtures F7a/F7b): does the proxy rise with the load?
Check s2_proxy(const TraceReader &trace, const Blocks &blocks, int min_blocks = 50);

// Returns true if the trace was recorded on REAPER's dummy audio device.
// Timer-driven callbacks that never miss a deadline (spike S2).
bool dummy_device(const TraceReader &trace);

// Times of the host's own xruns while recording.
std::vector<double> xrun_times(const TraceReader &trace, std::string_view kind = "audio_xrun");

}  // namespace sessionscope

#endif  // SESSIONSCOPE_CHECKS_H
