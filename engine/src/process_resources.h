#ifndef SESSIONSCOPE_PROCESS_RESOURCES_H
#define SESSIONSCOPE_PROCESS_RESOURCES_H

// =================================================================================================
// process_resources.h - the DAW's memory and disk reads, and the computer's free memory
// =================================================================================================
//
// For the HUD's "Memory and disk" module: how much memory (RAM) the DAW holds, how much the
// computer has left, and how fast the DAW reads (sample libraries streaming from disk).
//
// Where it comes from (all documented by Microsoft / Apple under these names):
//   Windows: GetProcessMemoryInfo (the DAW's "working set": its memory that is in RAM now),
//     GetProcessIoCounters (bytes read: from disk mostly, but also pipes and network, which a DAW
//     rarely uses), GlobalMemoryStatusEx (the computer's total and available memory).
//   macOS: proc_pid_rusage (physical footprint, disk bytes read), host_statistics64 (free memory).

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
