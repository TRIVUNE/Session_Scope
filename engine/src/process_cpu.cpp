// Process CPU reading implementation for Windows, Linux, and macOS.

#include "process_cpu.h"

#include "sensor_ring.h"   // now_seconds(), and <windows.h> on Windows

#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

namespace sessionscope {

ProcessCpu::~ProcessCpu() {
    close_process_handle();
}

bool ProcessCpu::follow(double process_id) {
    // Anything that is not a plausible process id counts as "no process". The id comes from shared
    // memory, and converting an out-of-range double to an integer is undefined behaviour.
    if (!(process_id > 0.0 && process_id <= 4294967295.0)) {
        close_process_handle();
        pid_ = 0.0;
        exe_name_.clear();
        have_baseline_ = false;
        return false;
    }

    // If the pid hasn't changed, keep the current state.
    if (process_id == pid_ && handle_ != kInvalidHandle) {
        return true;
    }

    // New or changed pid: close old handle and try to open the new one.
    close_process_handle();
    pid_ = process_id;
    exe_name_.clear();
    have_baseline_ = false;
    last_kernel_ns_ = 0;
    last_user_ns_ = 0;
    last_wall_s_ = 0.0;

    if (!open_process_handle(pid_)) {
        pid_ = 0.0;
        return false;
    }

    // Read the executable name once.
    std::string name;
    if (read_executable_name(name)) {
        exe_name_ = std::move(name);
    }

    return true;
}

double ProcessCpu::sample() {
    if (handle_ == kInvalidHandle) return std::numeric_limits<double>::quiet_NaN();
    const double now = now_seconds();

    // The first call only records a starting point: a share needs two readings.
    if (!have_baseline_) {
        uint64_t kernel_ns = 0, user_ns = 0;
        if (read_cpu_times(kernel_ns, user_ns) && is_process_running()) {
            last_kernel_ns_ = kernel_ns;
            last_user_ns_ = user_ns;
            last_wall_s_ = now;
            have_baseline_ = true;
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Require at least 1 ms of wall time to avoid noise.
    if (now - last_wall_s_ < 0.001) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Read current CPU times.
    uint64_t kernel_ns = 0, user_ns = 0;
    if (!read_cpu_times(kernel_ns, user_ns) || !is_process_running()) {
        close_process_handle();
        have_baseline_ = false;
        return std::numeric_limits<double>::quiet_NaN();
    }

    const uint64_t delta_cpu_ns = (kernel_ns - last_kernel_ns_) + (user_ns - last_user_ns_);
    const double delta_wall_s = now - last_wall_s_;
    const int cores = cpu_core_count();

    // Update baseline for next call.
    last_kernel_ns_ = kernel_ns;
    last_user_ns_ = user_ns;
    last_wall_s_ = now;

    if (cores <= 0 || delta_wall_s <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // CPU share = (CPU time used) / (wall time * core count).
    // delta_cpu_ns is in nanoseconds; delta_wall_s is in seconds.
    const double cpu_share = static_cast<double>(delta_cpu_ns) * 1e-9 / (delta_wall_s * cores);

    // Clamp to [0, 1].
    if (cpu_share <= 0.0) return 0.0;
    if (cpu_share > 1.0) return 1.0;
    return cpu_share;
}

int ProcessCpu::cpu_core_count() {
#ifdef _WIN32
    return static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#elif defined(__APPLE__) || defined(__linux__)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
#else
    return 1;
#endif
}

// -------------------------- Windows implementation --------------------------

#ifdef _WIN32

bool ProcessCpu::open_process_handle(double pid) {
    DWORD dw_pid = static_cast<DWORD>(pid);
    // PROCESS_QUERY_LIMITED_INFORMATION is the minimal access right needed for
    // GetProcessTimes and QueryFullProcessImageNameW.
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dw_pid);
    if (!h) {
        return false;
    }
    handle_ = h;
    return true;
}

void ProcessCpu::close_process_handle() {
    if (handle_ != kInvalidHandle) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = kInvalidHandle;
    }
}

bool ProcessCpu::read_cpu_times(uint64_t &out_kernel_ns, uint64_t &out_user_ns) {
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(static_cast<HANDLE>(handle_), &creation, &exit, &kernel, &user)) {
        return false;
    }
    // FILETIME is 100-ns intervals since Jan 1 1601. Convert to nanoseconds.
    auto filetime_to_ns = [](const FILETIME &ft) -> uint64_t {
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart * 100ULL;  // 100 ns -> ns
    };
    out_kernel_ns = filetime_to_ns(kernel);
    out_user_ns = filetime_to_ns(user);
    return true;
}

bool ProcessCpu::read_executable_name(std::string &out_name) {
    wchar_t wide_path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(static_cast<HANDLE>(handle_), 0, wide_path, &size)) {
        return false;
    }
    // Convert to UTF-8.
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return false;
    std::string utf8(needed - 1, '\0');  // exclude null terminator
    WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, utf8.data(), needed, nullptr, nullptr);
    // Keep only the file name (after the last backslash).
    size_t pos = utf8.find_last_of('\\');
    if (pos != std::string::npos) {
        out_name = utf8.substr(pos + 1);
    } else {
        out_name = std::move(utf8);
    }
    return true;
}

bool ProcessCpu::is_process_running() {
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(handle_), &exit_code)) {
        return false;
    }
    return exit_code == STILL_ACTIVE;
}

#endif  // _WIN32

// -------------------------- macOS implementation --------------------------

#ifdef __APPLE__

bool ProcessCpu::open_process_handle(double pid) {
    // On macOS, we don't need a persistent handle for proc_pidinfo.
    // Just verify the process exists by trying to read its info.
    struct proc_taskinfo ti;
    int res = proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
    if (res != sizeof(ti)) {
        return false;
    }
    handle_ = static_cast<int>(pid);
    return true;
}

void ProcessCpu::close_process_handle() {
    handle_ = kInvalidHandle;
}

bool ProcessCpu::read_cpu_times(uint64_t &out_kernel_ns, uint64_t &out_user_ns) {
    int pid = handle_;
    struct proc_taskinfo ti;
    int res = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
    if (res != sizeof(ti)) {
        return false;
    }
    // pti_total_user and pti_total_system are in mach absolute time units.
    // Convert to nanoseconds using the timebase.
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) {
        mach_timebase_info(&tb);
    }
    uint64_t user_ns = ti.pti_total_user * tb.numer / tb.denom;
    uint64_t kernel_ns = ti.pti_total_system * tb.numer / tb.denom;
    out_user_ns = user_ns;
    out_kernel_ns = kernel_ns;
    return true;
}

bool ProcessCpu::read_executable_name(std::string &out_name) {
    int pid = handle_;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int res = proc_pidpath(pid, path, sizeof(path));
    if (res <= 0) {
        return false;
    }
    std::string full(path);
    size_t pos = full.find_last_of('/');
    if (pos != std::string::npos) {
        out_name = full.substr(pos + 1);
    } else {
        out_name = std::move(full);
    }
    return true;
}

bool ProcessCpu::is_process_running() {
    int pid = handle_;
    struct proc_taskinfo ti;
    int res = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
    return res == sizeof(ti);
}

#endif  // __APPLE__

// -------------------------- Linux implementation --------------------------

#if defined(__linux__)

bool ProcessCpu::open_process_handle(double pid) {
    // On Linux, we just store the pid and verify /proc/<pid>/stat is readable.
    int int_pid = static_cast<int>(pid);
    std::string path = "/proc/" + std::to_string(int_pid) + "/stat";
    std::ifstream test(path);
    if (!test) {
        return false;
    }
    handle_ = int_pid;
    return true;
}

void ProcessCpu::close_process_handle() {
    handle_ = kInvalidHandle;
}

bool ProcessCpu::read_cpu_times(uint64_t &out_kernel_ns, uint64_t &out_user_ns) {
    int pid = static_cast<int>(handle_);
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    std::getline(in, line);
    if (line.empty()) {
        return false;
    }
    // The process name is in parentheses and may contain spaces.
    // Find the closing ')' and parse fields after it.
    size_t paren = line.rfind(')');
    if (paren == std::string::npos) {
        return false;
    }
    std::string after = line.substr(paren + 1);
    std::istringstream iss(after);
    // After the name come state (a letter), then ppid, pgrp, session, tty_nr, tpgid, flags, minflt,
    // cminflt, majflt, cmajflt, and only then utime and stime: fields 14 and 15 in proc(5).
    std::string state;
    if (!(iss >> state)) return false;
    long long skipped = 0;
    for (int field = 0; field < 10; ++field) {
        if (!(iss >> skipped)) return false;
    }
    unsigned long long utime, stime;
    if (!(iss >> utime >> stime)) {
        return false;
    }
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    // Convert clock ticks to nanoseconds.
    out_user_ns = static_cast<uint64_t>(utime) * 1000000000ULL / static_cast<uint64_t>(clk_tck);
    out_kernel_ns = static_cast<uint64_t>(stime) * 1000000000ULL / static_cast<uint64_t>(clk_tck);
    return true;
}

bool ProcessCpu::read_executable_name(std::string &out_name) {
    int pid = static_cast<int>(handle_);
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::getline(in, out_name);
    // /proc/pid/comm returns the name with a trailing newline, no path.
    if (!out_name.empty() && out_name.back() == '\n') {
        out_name.pop_back();
    }
    return !out_name.empty();
}

bool ProcessCpu::is_process_running() {
    int pid = static_cast<int>(handle_);
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream test(path);
    return static_cast<bool>(test);
}

#endif  // __linux__

}  // namespace sessionscope
