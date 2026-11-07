#ifndef SESSIONSCOPE_PROCESS_CPU_H
#define SESSIONSCOPE_PROCESS_CPU_H

// Reads a process's total CPU time (kernel + user) and reports it as a share of the whole
// machine's capacity. This is the same idea as Task Manager's CPU column: it says how busy the
// DAW process is overall. It is NOT audio-thread load; it includes all threads in the process.
// The value is a fraction of all cores: 1.0 means every core fully busy on this process.
// Returns NaN when the process is unknown, on the first sample (no baseline yet), when less
// than 1 ms has passed since the last sample, or when the process has exited.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace sessionscope {

class ProcessCpu {
public:
    ProcessCpu() = default;
    ~ProcessCpu();

    ProcessCpu(const ProcessCpu &) = delete;
    ProcessCpu &operator=(const ProcessCpu &) = delete;

    // Start following the process with the given id. An id of 0, NaN, negative, or a process
    // that cannot be opened gives false and the object simply reports nothing: this is a normal
    // state, not an error. A changed id closes the old process and resets the baseline.
    bool follow(double process_id);

    // CPU use since the previous call, as a share of the whole machine: 1.0 means every core
    // fully busy on this process. Returns NaN when nothing can be read, on the first call after
    // follow() because there is no baseline yet, when less than 1 ms has passed, or when the
    // process has exited. Never throws. Never negative. Clamped to at most 1.0.
    double sample();

    // File name only, such as "Ableton Live 12 Suite.exe" (UTF-8), or empty when unknown.
    // Read once in follow().
    const std::string &executable_name() const { return exe_name_; }

private:
    // Platform-specific handle type and helpers.
#ifdef _WIN32
    using Handle = void *;          // HANDLE
    static constexpr Handle kInvalidHandle = nullptr;
#else
    using Handle = int;
    static constexpr Handle kInvalidHandle = -1;
#endif

    // Platform-specific functions implemented in process_cpu.cpp.
    bool open_process_handle(double pid);
    void close_process_handle();
    bool read_cpu_times(uint64_t &out_kernel_ns, uint64_t &out_user_ns);
    bool read_executable_name(std::string &out_name);
    bool is_process_running();
    static int cpu_core_count();

    Handle handle_ = kInvalidHandle;
    double pid_ = 0.0;
    std::string exe_name_;
    uint64_t last_kernel_ns_ = 0;
    uint64_t last_user_ns_ = 0;
    double last_wall_s_ = 0.0;
    bool have_baseline_ = false;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_PROCESS_CPU_H
