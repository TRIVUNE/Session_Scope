#ifndef SESSIONSCOPE_PROCESS_RESOURCES_H
#define SESSIONSCOPE_PROCESS_RESOURCES_H

// Memory and disk for the HUD's "Memory and disk" module: how much RAM the DAW process holds, how
// much the machine has left, and how fast the DAW reads (sample libraries streaming from disk).
//
// Windows: GetProcessMemoryInfo (working set), GetProcessIoCounters (bytes read: disk mostly, but
//   also pipes and network, which a DAW rarely uses), GlobalMemoryStatusEx.
// macOS: proc_pid_rusage (physical footprint, disk bytes read), host_statistics64.

#include <optional>

namespace sessionscope {

struct SystemMemory {
    std::optional<double> total_bytes;
    std::optional<double> available_bytes;   // free for programs without swapping
};
SystemMemory system_memory();

class ProcessResources {
public:
    ProcessResources() = default;
    ~ProcessResources();
    ProcessResources(const ProcessResources &) = delete;
    ProcessResources &operator=(const ProcessResources &) = delete;

    // Follows the process with this id; 0 or an unknown process follows nothing (not an error).
    bool follow(double process_id);

    struct Reading {
        std::optional<double> memory_bytes;          // RAM the process holds
        std::optional<double> read_bytes_per_s;      // since the previous sample; none the first time
    };
    // `now` in seconds on any steady clock.
    Reading sample(double now);

private:
    void close();
    bool read(double &memory_bytes, double &read_bytes) const;

#ifdef _WIN32
    void *handle_ = nullptr;   // HANDLE
#endif
    double pid_ = 0;
    std::optional<double> last_read_bytes_;
    double last_t_ = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_PROCESS_RESOURCES_H
