#ifndef SESSIONSCOPE_CORE_CPU_H
#define SESSIONSCOPE_CORE_CPU_H

// How busy each logical core of the machine is, like Task Manager's per-core graphs. It counts
// every process, not only the DAW: a core pinned by the audio thread shows as one full core
// even when the DAW's overall share of the machine is small.
//
// Windows: NtQuerySystemInformation(SystemProcessorPerformanceInformation), the source Task
//   Manager uses. It covers the cores of the calling thread's processor group: all of them on
//   machines with up to 64 logical cores.
// macOS: host_processor_info(PROCESSOR_CPU_LOAD_INFO).

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
