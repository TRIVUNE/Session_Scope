#include "cpu_monitor.h"

#include <cmath>

namespace sessionscope {

void CpuMonitor::tick(double now, double host_process_id, std::optional<int> bar,
                      std::optional<double> stand_in_daw) {
    CpuSample sample;
    sample.t = now;
    sample.bar = bar;
    for (const double share : cores_.sample()) sample.cores.push_back(static_cast<float>(share));

    if (stand_in_daw) {
        sample.daw = *stand_in_daw;
    } else {
        if (host_process_id != followed_process_id_) {   // a new DAW (or none): start over
            daw_.follow(host_process_id);
            followed_process_id_ = host_process_id;
        }
        const double share = daw_.sample();   // NaN on the first reading or with no DAW
        if (!std::isnan(share)) sample.daw = share;
    }

    samples_.push_back(std::move(sample));
    while (!samples_.empty() && samples_.front().t < now - span_s_) samples_.pop_front();
}

int CpuMonitor::core_count() const {
    return samples_.empty() ? 0 : static_cast<int>(samples_.back().cores.size());
}

}  // namespace sessionscope
