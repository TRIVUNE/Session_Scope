// process_resources.cpp - the memory and disk reads of the DAW, and the free memory of the
// computer (see process_resources.h for the system calls used).

#include "process_resources.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif

namespace sessionscope {

ProcessResources::~ProcessResources() { close(); }

ProcessResources::Reading ProcessResources::sample(double now) {
    Reading reading;
    double memory = 0, read_bytes = 0;
    if (pid_ <= 0 || !read(memory, read_bytes)) {
        last_read_bytes_.reset();
        return reading;
    }
    reading.memory_bytes = memory;
    if (last_read_bytes_ && now > last_t_ && read_bytes >= *last_read_bytes_) {
        reading.read_bytes_per_s = (read_bytes - *last_read_bytes_) / (now - last_t_);
    }
    last_read_bytes_ = read_bytes;
    last_t_ = now;
    return reading;
}

#ifdef _WIN32

bool ProcessResources::follow(double process_id) {
    close();
    if (!(process_id > 0) || process_id > 4294967295.0) return false;
    const auto pid = static_cast<DWORD>(process_id);
    // Memory needs VM_READ as well; reading only the I/O counters works without it.
    handle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!handle_) handle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle_) return false;
    pid_ = process_id;
    return true;
}

void ProcessResources::close() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    pid_ = 0;
    last_read_bytes_.reset();
}

bool ProcessResources::read(double &memory_bytes, double &read_bytes) const {
    const auto process = static_cast<HANDLE>(handle_);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) return false;
    IO_COUNTERS io;
    if (!GetProcessIoCounters(process, &io)) return false;
    read_bytes = static_cast<double>(io.ReadTransferCount);
    PROCESS_MEMORY_COUNTERS memory;
    memory_bytes = K32GetProcessMemoryInfo(process, &memory, sizeof memory) ? static_cast<double>(memory.WorkingSetSize)
                                                                             : std::nan("");
    return !std::isnan(memory_bytes);
}

SystemMemory system_memory() {
    SystemMemory out;
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        out.total_bytes = static_cast<double>(status.ullTotalPhys);
        out.available_bytes = static_cast<double>(status.ullAvailPhys);
    }
    return out;
}

#elif defined(__APPLE__)

bool ProcessResources::follow(double process_id) {
    close();
    if (!(process_id > 0)) return false;
    pid_ = process_id;
    return true;
}

void ProcessResources::close() {
    pid_ = 0;
    last_read_bytes_.reset();
}

bool ProcessResources::read(double &memory_bytes, double &read_bytes) const {
    rusage_info_v2 info;
    if (proc_pid_rusage(static_cast<int>(pid_), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t *>(&info)) != 0) {
        return false;
    }
    memory_bytes = static_cast<double>(info.ri_phys_footprint);
    read_bytes = static_cast<double>(info.ri_diskio_bytesread);
    return true;
}

SystemMemory system_memory() {
    SystemMemory out;
    std::uint64_t total = 0;
    std::size_t size = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0) out.total_bytes = static_cast<double>(total);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        // Free plus inactive pages: what Activity Monitor counts as available without pressure.
        out.available_bytes = static_cast<double>(vm.free_count + vm.inactive_count) * static_cast<double>(page);
    }
    return out;
}

#else

bool ProcessResources::follow(double) { return false; }
void ProcessResources::close() {}
bool ProcessResources::read(double &, double &) const { return false; }
SystemMemory system_memory() { return {}; }

#endif

}  // namespace sessionscope
