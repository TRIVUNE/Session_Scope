#ifndef SESSIONSCOPE_CORE_CPU_H
#define SESSIONSCOPE_CORE_CPU_H

// =================================================================================================
// core_cpu.h - how busy each core of the computer is
// =================================================================================================
//
// Like Task Manager's per-core graphs. It counts every program, not only the DAW: a core kept full
// by the audio thread shows as one full core even when the DAW's share of the whole computer is
// small. ("Logical cores": with hyper-threading, each physical core counts as two.)
//
// How: the system keeps, for every core, a running total of time spent idle and time spent busy.
// We read the totals, and next time read them again: busy time added / all time added = how busy
// the core was in between.
//
// Where it comes from:
//   Windows: NtQuerySystemInformation(SystemProcessorPerformanceInformation), from ntdll.dll, the
//     source Task Manager uses (documented by Microsoft under "NtQuerySystemInformation"). It
//     covers the cores of the calling thread's processor group: all of them on machines with up to
//     64 logical cores.
//   macOS: host_processor_info(PROCESSOR_CPU_LOAD_INFO), Apple's Mach kernel interface.

#include <cstdint>
#include <vector>

namespace sessionscope {

class CoreCpu {
public:
    CoreCpu() = default;

    // Each core's busy share since the previous call, 0..1, in the system's core order. Empty on
    // the first call (no baseline yet) or if the system can't be read. Never throws.
    std::vector<double> sample();

private:
    struct Times {
        std::uint64_t busy = 0;    // time doing anything but idling
        std::uint64_t total = 0;   // busy + idle
    };
    // The system's cumulative per-core times; false if they can't be read.
    static bool read(std::vector<Times> &out);

    std::vector<Times> last_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_CORE_CPU_H
