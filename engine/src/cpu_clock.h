#ifndef SESSIONSCOPE_CPU_CLOCK_H
#define SESSIONSCOPE_CPU_CLOCK_H

// The CPU's speed right now against its rated speed, for the HUD's "CPU clock" module. A laptop on
// battery or a hot PC slows its cores down mid-session, and the same plug-ins then need more of
// each block: a common hidden cause of dropouts.
//
// Windows: the "Processor Information" performance counters Task Manager uses (% Processor
//   Performance x Processor Frequency = current speed, which can pass the rated speed with turbo;
//   % Performance Limit below 100 means the system is holding the CPU back). pdh.dll is loaded at
//   run time, so nothing needs to link it.
// macOS: no public interface gives the current speed; every reading is empty.

#include <memory>
#include <optional>

namespace sessionscope {

class CpuClock {
public:
    CpuClock();
    ~CpuClock();
    CpuClock(const CpuClock &) = delete;
    CpuClock &operator=(const CpuClock &) = delete;

    struct Reading {
        std::optional<double> mhz;          // current average speed of the cores
        std::optional<double> rated_mhz;    // the rated (base) speed
        std::optional<double> limit;        // 0..1: share of full speed the system allows; 1 = no limit
    };
    // Since the previous call; empty the first time (the counters need two samples).
    Reading sample();

private:
    struct Counters;
    std::unique_ptr<Counters> counters_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_CPU_CLOCK_H
