// =================================================================================================
// probe_monitor.cpp - CPU per track and per plug-in (see probe_monitor.h)
// =================================================================================================

#include "probe_monitor.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace sessionscope {

namespace {

// Readings kept for the "last second" average: ten readings a second.
constexpr std::size_t WINDOW = 10;

// Adds `value` to the end of `list`, keeping at most WINDOW entries.
template <class T>
void push_window(std::vector<T> &list, T value) {
    list.push_back(value);
    if (list.size() > WINDOW) list.erase(list.begin());
}

}  // namespace

ProbeMonitor::ProbeMonitor(const char *table_name, bool take_peaks)
    : table_(probes::open_table(table_name)), take_peaks_(take_peaks) {}

void ProbeMonitor::tick(double daw_process_id) {
    tracks_.clear();
    if (!table_) return;
    const std::uint64_t now = probes::now_ns();
    std::map<std::uint64_t, Previous> still_running;   // probes seen this time; the rest are forgotten

    for (probes::Slot &slot : table_->slot) {
        const std::uint64_t owner = slot.owner.load();
        if (owner == 0) continue;                                        // free slot
        if (now - slot.heartbeat_ns.load() > probes::STALE_NS) continue;   // its probe went silent

        // Names: copy them, and keep the copy only if no write happened meanwhile (see the
        // sequence counter in probe_table.h). Otherwise skip this probe until the next reading.
        const std::uint64_t version = slot.names_version.load(std::memory_order_acquire);
        if (version % 2 != 0) continue;
        char track[probes::NAME_BYTES], plugin[probes::NAME_BYTES];
        std::memcpy(track, slot.track, sizeof track);
        std::memcpy(plugin, slot.plugin, sizeof plugin);
        const std::int64_t order = slot.track_order;
        const std::uint64_t process = slot.process_id;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.names_version.load() != version) continue;
        track[probes::NAME_BYTES - 1] = plugin[probes::NAME_BYTES - 1] = '\0';   // always ends in a zero
        if (daw_process_id > 0 && process != 0 && static_cast<double>(process) != daw_process_id) continue;

        // The counters, and their growth since the last reading.
        const std::uint64_t cost = slot.cost_ns.load(), budget = slot.budget_ns.load();
        // The worst block: taken (set back to 0, so the next reading starts fresh) or only looked at.
        const std::uint64_t peak_permille = take_peaks_ ? slot.peak_permille.exchange(0) : slot.peak_permille.load();
        const double peak = static_cast<double>(peak_permille) / 1000.0;
        Previous p = previous_.count(owner) ? previous_[owner] : Previous{cost, budget, {}, {}, {}};
        push_window(p.costs, cost >= p.cost_ns ? cost - p.cost_ns : 0);
        push_window(p.budgets, budget >= p.budget_ns ? budget - p.budget_ns : 0);
        push_window(p.peaks, peak);
        p.cost_ns = cost;
        p.budget_ns = budget;
        still_running[owner] = p;

        // Over the last second: total cost / total budget, and the worst block.
        const std::uint64_t window_cost = std::accumulate(p.costs.begin(), p.costs.end(), std::uint64_t{0});
        const std::uint64_t window_budget = std::accumulate(p.budgets.begin(), p.budgets.end(), std::uint64_t{0});
        PluginLoad load;
        load.name = plugin[0] ? plugin : "(plug-in)";
        load.cpu = window_budget ? static_cast<double>(window_cost) / static_cast<double>(window_budget) : 0;
        load.peak = *std::max_element(p.peaks.begin(), p.peaks.end());
        load.slot_owner = owner;
        load.blocks = slot.blocks.load();
        load.process_id = process;

        // Into its track (by name; a probe without a track name gets a track of its own).
        const std::string track_name = track[0] ? track : "(track)";
        auto found = std::find_if(tracks_.begin(), tracks_.end(), [&](const TrackLoad &t) { return t.name == track_name; });
        if (found == tracks_.end()) {
            tracks_.push_back(TrackLoad{track_name, order, 0, 0, {}});
            found = tracks_.end() - 1;
        }
        found->cpu += load.cpu;
        // A track's peak: its plug-ins' worst blocks added up. An upper bound: each probe only
        // sees its own blocks, so whether those worst blocks were the same block is unknown.
        found->peak += load.peak;
        if (order >= 0 && (found->order < 0 || order < found->order)) found->order = order;
        found->plugins.push_back(load);
    }
    previous_ = std::move(still_running);

    // DAW order first; tracks without a known position after them, by name.
    std::sort(tracks_.begin(), tracks_.end(), [](const TrackLoad &a, const TrackLoad &b) {
        if ((a.order < 0) != (b.order < 0)) return a.order >= 0;
        if (a.order != b.order) return a.order < b.order;
        return a.name < b.name;
    });
}

}  // namespace sessionscope
