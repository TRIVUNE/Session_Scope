// Tests for CoreCpu (per-core load of this machine), SystemMonitor (the HUD's readings), the
// DAW process's memory and disk, and the machine's memory.

#include "core_cpu.h"
#include "system_monitor.h"
#include "sensor_ring.h"   // process_id()
#include "test_support.h"

#include <chrono>
#include <cstdint>
#include <thread>

using namespace sessionscope;

namespace {

// Keeps one core busy for about `ms` milliseconds.
void burn(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    volatile std::uint64_t spins = 0;   // volatile: the compiler must keep the loop
    while (std::chrono::steady_clock::now() < until) spins = spins + 1;
}

}  // namespace

int main() {
    // CoreCpu: nothing on the first call, then one share per core, each in 0..1.
    {
        CoreCpu cores;
        CHECK(cores.sample().empty());
        burn(200);
        const std::vector<double> shares = cores.sample();
        CHECK(!shares.empty());
        double busiest = 0;
        for (const double share : shares) {
            CHECK(share >= 0.0 && share <= 1.0);
            if (share > busiest) busiest = share;
        }
        CHECK(busiest > 0.5);   // the burning core was busy for most of the time
    }

    // SystemMonitor follows a process by id: this test's own, which is busy.
    {
        SystemMonitor monitor(1.0);
        monitor.tick(0.0, process_id(), std::nullopt);
        CHECK(monitor.samples().size() == 1);
        CHECK(!monitor.samples().back().daw);          // no baseline yet
        CHECK(monitor.samples().back().cores.empty());
        burn(150);
        monitor.tick(0.15, process_id(), 7);
        CHECK(monitor.samples().back().daw.has_value());
        CHECK(*monitor.samples().back().daw > 0.0);
        CHECK(monitor.samples().back().bar == 7);
        CHECK(monitor.core_count() > 0);
    }

    // A stand-in value replaces the DAW's CPU (the demo); readings older than the span go.
    {
        SystemMonitor monitor(1.0);
        for (int i = 0; i <= 30; ++i) monitor.tick(i * 0.1, 0, i / 10, 0.25);
        CHECK(monitor.samples().front().t >= 2.0 - 1e-9);
        CHECK(monitor.samples().size() == 11);
        CHECK(monitor.samples().back().daw == 0.25);
        CHECK(monitor.samples().back().bar == 3);
    }

    // No DAW known: no DAW value.
    {
        SystemMonitor monitor;
        monitor.tick(0.0, 0, std::nullopt);
        monitor.tick(0.1, 0, std::nullopt);
        CHECK(!monitor.samples().back().daw);
        CHECK(!monitor.samples().back().daw_memory_bytes);
    }

    // The machine's memory: some total, and less than that available.
    {
        const SystemMemory memory = system_memory();
        CHECK(memory.total_bytes && *memory.total_bytes > 0);
        CHECK(memory.available_bytes && *memory.available_bytes <= *memory.total_bytes);
    }

    // A process's memory and reads: this test's own. The read rate may be zero: the
    // test reads nothing between the two samples.
    {
        ProcessResources resources;
        CHECK(resources.follow(process_id()));
        const ProcessResources::Reading first = resources.sample(0.0);
        CHECK(first.memory_bytes && *first.memory_bytes > 0);
        CHECK(!first.read_bytes_per_s);   // no baseline yet
        const ProcessResources::Reading second = resources.sample(0.5);
        CHECK(second.read_bytes_per_s && *second.read_bytes_per_s >= 0);
        CHECK(!resources.follow(0));
        CHECK(!resources.sample(1.0).memory_bytes);
    }

    // The CPU clock: on Windows a speed and a rated speed once two samples are taken.
    {
        CpuClock clock;
        clock.sample();
        burn(200);
        const CpuClock::Reading reading = clock.sample();
#ifdef _WIN32
        CHECK(reading.rated_mhz && *reading.rated_mhz > 100);
        CHECK(reading.mhz && *reading.mhz > 100);
        CHECK(reading.limit && *reading.limit > 0);
#else
        CHECK(!reading.mhz);
#endif
    }

    return finish_tests();
}
