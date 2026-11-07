#include "cpu_clock.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace sessionscope {

#ifdef _WIN32

// pdh.dll's functions, looked up at run time, and the query with its three counters.
struct CpuClock::Counters {
    using OpenQuery = PDH_STATUS(WINAPI *)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
    using AddCounter = PDH_STATUS(WINAPI *)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
    using Collect = PDH_STATUS(WINAPI *)(PDH_HQUERY);
    using Format = PDH_STATUS(WINAPI *)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
    using CloseQuery = PDH_STATUS(WINAPI *)(PDH_HQUERY);

    HMODULE library = nullptr;
    Collect collect = nullptr;
    Format format = nullptr;
    CloseQuery close_query = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER performance = nullptr;   // % Processor Performance: current speed / rated speed
    PDH_HCOUNTER frequency = nullptr;     // Processor Frequency: the rated speed, MHz
    PDH_HCOUNTER limit = nullptr;         // % Performance Limit

    template <class F>
    static F find(HMODULE module, const char *name) {
        return reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(module, name)));
    }

    bool open() {
        library = LoadLibraryW(L"pdh.dll");
        if (!library) return false;
        const auto open_query = find<OpenQuery>(library, "PdhOpenQueryW");
        const auto add = find<AddCounter>(library, "PdhAddEnglishCounterW");   // English names on any language of Windows
        collect = find<Collect>(library, "PdhCollectQueryData");
        format = find<Format>(library, "PdhGetFormattedCounterValue");
        close_query = find<CloseQuery>(library, "PdhCloseQuery");
        if (!open_query || !add || !collect || !format || !close_query) return false;
        if (open_query(nullptr, 0, &query) != ERROR_SUCCESS) return false;
        add(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &performance);
        add(query, L"\\Processor Information(_Total)\\Processor Frequency", 0, &frequency);
        add(query, L"\\Processor Information(_Total)\\% Performance Limit", 0, &limit);
        collect(query);   // the first sample: rates need a second one
        return true;
    }

    std::optional<double> value(PDH_HCOUNTER counter) const {
        if (!counter) return std::nullopt;
        PDH_FMT_COUNTERVALUE v;
        if (format(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS) return std::nullopt;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return std::nullopt;
        return v.doubleValue;
    }

    ~Counters() {
        if (query && close_query) close_query(query);
        if (library) FreeLibrary(library);
    }
};

CpuClock::CpuClock() : counters_(std::make_unique<Counters>()) {
    if (!counters_->open()) counters_.reset();
}

CpuClock::~CpuClock() = default;

CpuClock::Reading CpuClock::sample() {
    Reading reading;
    if (!counters_ || counters_->collect(counters_->query) != ERROR_SUCCESS) return reading;
    const std::optional<double> performance = counters_->value(counters_->performance);
    reading.rated_mhz = counters_->value(counters_->frequency);
    if (performance && reading.rated_mhz && *reading.rated_mhz > 0) reading.mhz = *reading.rated_mhz * *performance / 100.0;
    if (const std::optional<double> limit = counters_->value(counters_->limit)) reading.limit = *limit / 100.0;
    return reading;
}

#else

struct CpuClock::Counters {};
CpuClock::CpuClock() = default;
CpuClock::~CpuClock() = default;
CpuClock::Reading CpuClock::sample() { return {}; }

#endif

}  // namespace sessionscope
