#ifndef SESSIONSCOPE_CPU_CLOCK_H
#define SESSIONSCOPE_CPU_CLOCK_H

// =================================================================================================
// cpu_clock.h - the CPU's speed now, against its rated speed
// =================================================================================================
//
// For the HUD's "CPU clock" module. A laptop on battery or a hot PC slows its cores down
// mid-session, and the same plug-ins then need more of each audio block: a common hidden cause of
// dropouts.
//
// Where it comes from:
//   Windows: the "Processor Information" performance counters, the ones Task Manager's speed figure
//     uses, read through PDH (Performance Data Helper, pdh.dll; documented by Microsoft under
//     "Using the PDH Functions"):
//       % Processor Performance x Processor Frequency / 100 = the current speed (can be above the
//         rated speed with turbo boost)
//       % Performance Limit below 100 = the system is holding the CPU back
//     pdh.dll is loaded while the program runs (LoadLibrary), so nothing needs to link it.
//   macOS: there's no public way to read the current speed; every reading is empty.

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
