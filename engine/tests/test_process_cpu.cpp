// Tests for ProcessCpu class.
// Uses this test process's own PID to verify basic functionality.

#include "process_cpu.h"
#include "test_support.h"
#include "sensor_ring.h"   // for process_id() and now_seconds()

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>

using namespace sessionscope;

int main() {
    ProcessCpu cpu;
    double own_pid = process_id();

    // follow(own id) returns true; executable_name() is not empty and contains "test_process_cpu"
    {
        CHECK(cpu.follow(own_pid));
        const std::string &name = cpu.executable_name();
        CHECK(!name.empty());
        CHECK(name.find("test_process_cpu") != std::string::npos);
    }

    // the first sample() is NaN
    {
        double v = cpu.sample();
        CHECK(std::isnan(v));
    }

    // burn CPU in a busy loop for about 300 ms on one thread, then sample():
    // the value is above 0 and at most 1.0, and on an N-core machine at least 0.5 / N
    {
        // Busy-wait for ~300 ms.
        volatile std::uint64_t spins = 0;   // volatile: the compiler must keep this loop
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300)) {
            spins = spins + 1;
        }
        double v = cpu.sample();
        CHECK(!std::isnan(v));
        CHECK(v > 0.0);
        CHECK(v <= 1.0);

        // One thread busy for the whole period is one core's worth. Half of that leaves room for a
        // busy machine, while a wrong unit or a wrong core count would still fail.
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        CHECK(v >= 0.5 / cores);
    }

    // An id too large for any process must not reach an integer conversion
    CHECK(!cpu.follow(1e300));

    // follow(0), follow(NaN), follow(-5) and follow(a huge id such as 4.0e9) return false,
    // executable_name() is empty, and sample() is NaN
    {
        CHECK(!cpu.follow(0.0));
        CHECK(cpu.executable_name().empty());
        CHECK(std::isnan(cpu.sample()));

        CHECK(!cpu.follow(std::numeric_limits<double>::quiet_NaN()));
        CHECK(cpu.executable_name().empty());
        CHECK(std::isnan(cpu.sample()));

        CHECK(!cpu.follow(-5.0));
        CHECK(cpu.executable_name().empty());
        CHECK(std::isnan(cpu.sample()));

        CHECK(!cpu.follow(4.0e9));
        CHECK(cpu.executable_name().empty());
        CHECK(std::isnan(cpu.sample()));
    }

    // following the own id again after that works again
    {
        CHECK(cpu.follow(own_pid));
        CHECK(!cpu.executable_name().empty());
        CHECK(cpu.executable_name().find("test_process_cpu") != std::string::npos);
        double v = cpu.sample();  // first sample after follow is NaN
        CHECK(std::isnan(v));
        // Give it a moment and sample again.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        v = cpu.sample();
        CHECK(!std::isnan(v));
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
    }

    return finish_tests();
}
