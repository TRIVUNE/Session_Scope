// Tests for CoreCpu (per-core load of this machine) and CpuMonitor (the HUD strip's history).

#include "core_cpu.h"
#include "cpu_monitor.h"
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

    // CpuMonitor follows a process by id: this test's own, which is busy.
    {
        CpuMonitor monitor(1.0);
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
        CpuMonitor monitor(1.0);
        for (int i = 0; i <= 30; ++i) monitor.tick(i * 0.1, 0, i / 10, 0.25);
        CHECK(monitor.samples().front().t >= 2.0 - 1e-9);
        CHECK(monitor.samples().size() == 11);
        CHECK(monitor.samples().back().daw == 0.25);
        CHECK(monitor.samples().back().bar == 3);
    }

    // No DAW known: no DAW value.
    {
        CpuMonitor monitor;
        monitor.tick(0.0, 0, std::nullopt);
        monitor.tick(0.1, 0, std::nullopt);
        CHECK(!monitor.samples().back().daw);
    }

    return finish_tests();
}
