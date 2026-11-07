#ifndef SESSIONSCOPE_PROBE_MONITOR_H
#define SESSIONSCOPE_PROBE_MONITOR_H

// =================================================================================================
// probe_monitor.h - CPU per track and per plug-in, from the SessionScope Probes
// =================================================================================================
//
// Reads the probes' shared table (native/sensor_core/probe_table.h) ten times a second and turns
// its running totals into what the Task Manager module shows: for every track, its plug-ins, and
// for each, the share of the audio block's time it used over the last second, and its worst block.
//
// How the share is worked out: each probe adds up the time it spent inside its plug-in (cost) and
// the real-time length of the blocks (budget). Between two readings:
//     CPU = (cost now - cost before) / (budget now - budget before)
// Kept apart from HudEngine for the same reason as SystemMonitor: it reads live data.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "probe_table.h"

namespace sessionscope {

// One wrapped plug-in.
struct PluginLoad {
    std::string name;
    double cpu = 0;    // share of the block time over the last second, 0..1 (can pass 1 when overloaded)
    double peak = 0;   // the worst single block in the last second, same scale
    std::uint64_t slot_owner = 0;   // stable identity while the probe runs (for the module's state)
};

// One track: its plug-ins, in the order their probes appeared, and their sums.
struct TrackLoad {
    std::string name;
    std::int64_t order = -1;   // position in the DAW, -1 if the host doesn't say
    double cpu = 0;            // sum of its plug-ins
    double peak = 0;           // its plug-ins' peaks added up: at most this (see probe_monitor.cpp)
    std::vector<PluginLoad> plugins;
};

class ProbeMonitor {
public:
    // `table_name`: the shared table to read; tests and the demo use their own.
    explicit ProbeMonitor(const char *table_name = probes::TABLE_NAME);

    // Reads the table. Call about ten times a second; `daw_process_id` (from the sensor) keeps
    // probes from another DAW out, 0 shows every probe.
    void tick(double daw_process_id = 0);

    // Tracks in DAW order (unknown order last, by name), with their plug-ins.
    const std::vector<TrackLoad> &tracks() const { return tracks_; }
    // True once at least one probe has reported.
    bool any() const { return !tracks_.empty(); }

private:
    // What we remember about a probe between readings, to take differences.
    struct Previous {
        std::uint64_t cost_ns = 0, budget_ns = 0;
        // The last second of differences, for a steady average: (cost, budget, peak) per reading.
        std::vector<std::uint64_t> costs, budgets;
        std::vector<double> peaks;
    };

    probes::Table *table_ = nullptr;
    std::map<std::uint64_t, Previous> previous_;   // by slot owner id
    std::vector<TrackLoad> tracks_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_PROBE_MONITOR_H
