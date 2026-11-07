#ifndef SESSIONSCOPE_SYSTEM_MONITOR_H
#define SESSIONSCOPE_SYSTEM_MONITOR_H

// The last few seconds of readings from the machine, for the HUD's modules: every core (the
// heatmap, the busiest core), the DAW process's CPU, memory and disk reads, the machine's free
// memory, the CPU's clock speed, and the bar that was playing at each reading (the bar lines).
//
// Kept apart from HudEngine on purpose: it reads the live machine, so HudEngine's snapshots stay
// reproducible from a trace.

#include <deque>
#include <optional>
#include <vector>

#include "core_cpu.h"
#include "cpu_clock.h"
#include "process_cpu.h"
#include "process_resources.h"

namespace sessionscope {

struct SystemSample {
    double t = 0;                              // seconds, on the clock passed to tick()
    std::vector<float> cores;                  // each core's busy share, 0..1; empty if unreadable
    std::optional<double> daw;                 // the DAW process's share of the whole machine, 0..1
    std::optional<double> daw_memory_bytes;    // RAM the DAW holds
    std::optional<double> daw_read_bytes_per_s;   // how fast the DAW reads (disk streaming)
    SystemMemory memory;                       // the machine's RAM
    CpuClock::Reading clock;                   // the CPU's speed now and its rated speed
    std::optional<int> bar;                    // the bar playing at t; none while stopped
};

class SystemMonitor {
public:
    explicit SystemMonitor(double span_s = 12.0) : span_s_(span_s) {}

    // Takes one reading. Call about ten times a second.
    //   host_process_id: the DAW's process (from the sensor), 0 if unknown.
    //   bar:             the bar playing now, if the host is playing.
    //   stand_in_daw:    a value to show instead of the DAW's CPU, for the demo (no DAW runs).
    void tick(double now, double host_process_id, std::optional<int> bar,
              std::optional<double> stand_in_daw = std::nullopt);

    // Oldest first; at most span_s seconds back from the newest.
    const std::deque<SystemSample> &samples() const { return samples_; }
    double span() const { return span_s_; }
    // How many cores the newest reading has (0 before the second tick).
    int core_count() const;

private:
    double span_s_;
    CoreCpu cores_;
    CpuClock clock_;
    ProcessCpu daw_;
    ProcessResources daw_resources_;
    double followed_process_id_ = 0;
    std::deque<SystemSample> samples_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_SYSTEM_MONITOR_H
