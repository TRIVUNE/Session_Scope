#include "core_cpu.h"

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace sessionscope {

std::vector<double> CoreCpu::sample() {
    std::vector<Times> now;
    if (!read(now)) {
        last_.clear();
        return {};
    }
    std::vector<double> shares;
    if (now.size() == last_.size()) {
        shares.reserve(now.size());
        for (std::size_t i = 0; i < now.size(); ++i) {
            const std::uint64_t busy = now[i].busy >= last_[i].busy ? now[i].busy - last_[i].busy : 0;
            const std::uint64_t total = now[i].total >= last_[i].total ? now[i].total - last_[i].total : 0;
            shares.push_back(total ? std::clamp(static_cast<double>(busy) / static_cast<double>(total), 0.0, 1.0) : 0.0);
        }
    }
    last_ = std::move(now);
    return shares;
}

#ifdef _WIN32

namespace {

// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, from winternl.h, in 100 ns units. KernelTime
// includes IdleTime.
struct ProcessorTimes {
    LARGE_INTEGER idle;
    LARGE_INTEGER kernel;
    LARGE_INTEGER user;
    LARGE_INTEGER dpc;
    LARGE_INTEGER interrupt;
    ULONG interrupt_count;
};

using NtQuerySystemInformationFn = LONG(WINAPI *)(int, void *, ULONG, ULONG *);
constexpr int SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION = 8;
constexpr std::size_t MAX_CORES_IN_GROUP = 64;

NtQuerySystemInformationFn query_function() {
    // Looked up once; ntdll is always loaded, so this never fails on a working Windows.
    static const auto function = reinterpret_cast<NtQuerySystemInformationFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    return function;
}

}  // namespace

bool CoreCpu::read(std::vector<Times> &out) {
    const NtQuerySystemInformationFn query = query_function();
    if (!query) return false;
    ProcessorTimes cores[MAX_CORES_IN_GROUP];
    ULONG written = 0;
    if (query(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, cores, sizeof(cores), &written) < 0) return false;
    const std::size_t count = written / sizeof(ProcessorTimes);
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto idle = static_cast<std::uint64_t>(cores[i].idle.QuadPart);
        const auto total = static_cast<std::uint64_t>(cores[i].kernel.QuadPart + cores[i].user.QuadPart);
        out[i].total = total;
        out[i].busy = total > idle ? total - idle : 0;
    }
    return count > 0;
}

#elif defined(__APPLE__)

bool CoreCpu::read(std::vector<Times> &out) {
    natural_t count = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t info_count = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &info_count) != KERN_SUCCESS) {
        return false;
    }
    out.resize(count);
    for (natural_t i = 0; i < count; ++i) {
        const integer_t *ticks = info + CPU_STATE_MAX * i;
        const auto user = static_cast<std::uint64_t>(ticks[CPU_STATE_USER]);
        const auto system = static_cast<std::uint64_t>(ticks[CPU_STATE_SYSTEM]);
        const auto nice = static_cast<std::uint64_t>(ticks[CPU_STATE_NICE]);
        const auto idle = static_cast<std::uint64_t>(ticks[CPU_STATE_IDLE]);
        out[i].busy = user + system + nice;
        out[i].total = out[i].busy + idle;
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), info_count * sizeof(integer_t));
    return count > 0;
}

#else

bool CoreCpu::read(std::vector<Times> &) { return false; }

#endif

}  // namespace sessionscope
