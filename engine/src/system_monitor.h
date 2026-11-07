#ifndef SESSIONSCOPE_SYSTEM_MONITOR_H
#define SESSIONSCOPE_SYSTEM_MONITOR_H

// =================================================================================================
// system_monitor.h - readings from this computer, for the HUD's modules
// =================================================================================================
//
// Ten times a second, tick() takes one reading and keeps the last 12 seconds of them. A reading
// holds:
//   - how busy each core is                 (CoreCpu, core_cpu.h)               the heatmap, busiest core
//   - the DAW's share of the whole computer (ProcessCpu, process_cpu.h)         peak meter, CPU number
//   - the DAW's memory and disk reads       (ProcessResources, process_resources.h)  memory module
//   - the computer's free memory            (system_memory(), process_resources.h)   memory module
//   - the CPU's speed                       (CpuClock, cpu_clock.h)             clock module
//   - the bar of the song playing           (from the engine)                   bar lines on the heatmap
//
// Memory, disk and speed change slowly and cost more to read (the speed goes through Windows'
// performance counters), so they are read only once a second; the readings in between repeat the
// last value.
//
// Kept apart from HudEngine on purpose: HudEngine only works from the sensor's data, so a recording
// played back gives exactly the same picture every time. This file reads the live computer.

#include <deque>
#include <optional>
#include <vector>

#include "core_cpu.h"
#include "cpu_clock.h"
#include "process_cpu.h"
#include "process_resources.h"

namespace sessionscope {

// One reading. std::optional means "may be missing": no DAW found, not readable on this system...
struct SystemSample {
    double t = 0;                                 // when, in seconds (the clock passed to tick())
    std::vector<float> cores;                     // each core's busy share 0..1; empty on the first reading
    std::optional<double> daw;                    // the DAW's share of the whole computer, 0..1
    std::optional<double> daw_memory_bytes;       // memory the DAW holds
    std::optional<double> daw_read_bytes_per_s;   // how fast the DAW reads (sample streaming from disk)
    SystemMemory memory;                          // the computer's total and free memory
    CpuClock::Reading clock;                      // the CPU's speed now, its rated speed, any limit
    std::optional<int> bar;                       // the bar of the song playing; empty while stopped
};

class SystemMonitor {
public:
    explicit SystemMonitor(double span_s = 12.0) : span_s_(span_s) {}

    // Takes one reading. Call about ten times a second.
    //   now              the time, in seconds, on any steady clock
    //   host_process_id  the DAW's process id (the sensor plug-in reports it); 0 if unknown
    //   bar              the bar playing now, if the DAW is playing
    //   stand_in_daw     a made-up DAW CPU for the demo, which has no DAW
    void tick(double now, double host_process_id, std::optional<int> bar,
              std::optional<double> stand_in_daw = std::nullopt);

    // The readings, oldest first; at most span() seconds back from the newest.
    const std::deque<SystemSample> &samples() const { return samples_; }
    double span() const { return span_s_; }
    // How many cores the newest reading has (0 until the second tick).
    int core_count() const;

private:
    // Memory, disk and speed: read once a second (see the top of this file).
    void read_slow_things(double now, SystemSample &sample);

    double span_s_;
    CoreCpu cores_;
    CpuClock clock_;
    ProcessCpu daw_;
    ProcessResources daw_resources_;
    double followed_process_id_ = 0;   // the DAW we're reading; 0 = none
    double slow_read_at_ = -1e9;       // when the slow things were last read
    SystemSample slow_;                // their last values, copied into every reading
    std::deque<SystemSample> samples_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_SYSTEM_MONITOR_H
