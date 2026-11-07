// =================================================================================================
// system_monitor.cpp - readings from this computer (see system_monitor.h)
// =================================================================================================

#include "system_monitor.h"

#include <cmath>

namespace sessionscope {

void SystemMonitor::tick(double now, double host_process_id, std::optional<int> bar,
                         std::optional<double> stand_in_daw) {
    SystemSample sample;
    sample.t = now;
    sample.bar = bar;
    for (const double share : cores_.sample()) sample.cores.push_back(static_cast<float>(share));

    // A different DAW (or none any more): start following the new one from scratch.
    if (host_process_id != followed_process_id_) {
        daw_.follow(host_process_id);
        daw_resources_.follow(host_process_id);
        followed_process_id_ = host_process_id;
        slow_read_at_ = -1e9;   // read the new DAW's memory at once, not up to a second later
    }

    // The DAW's CPU: every tick, it's what the meters show. ProcessCpu returns NaN ("not a number")
    // when it has nothing yet (the first reading, or no DAW).
    const double share = daw_.sample();
    if (stand_in_daw) sample.daw = *stand_in_daw;
    else if (!std::isnan(share)) sample.daw = share;

    read_slow_things(now, sample);

    samples_.push_back(std::move(sample));
    while (!samples_.empty() && samples_.front().t < now - span_s_) samples_.pop_front();   // forget readings past the span
}

void SystemMonitor::read_slow_things(double now, SystemSample &sample) {
    if (now - slow_read_at_ >= 1.0) {
        slow_read_at_ = now;
        const ProcessResources::Reading resources = daw_resources_.sample(now);
        slow_.daw_memory_bytes = resources.memory_bytes;
        slow_.daw_read_bytes_per_s = resources.read_bytes_per_s;   // averaged over the second since the last read
        slow_.memory = system_memory();
        slow_.clock = clock_.sample();
    }
    sample.daw_memory_bytes = slow_.daw_memory_bytes;
    sample.daw_read_bytes_per_s = slow_.daw_read_bytes_per_s;
    sample.memory = slow_.memory;
    sample.clock = slow_.clock;
}

int SystemMonitor::core_count() const {
    return samples_.empty() ? 0 : static_cast<int>(samples_.back().cores.size());
}

}  // namespace sessionscope
