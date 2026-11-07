// Tests for the shared-memory reader. They use private ring names, never the real ones, so they are
// safe to run while a DAW has the sensor loaded.

#include "ring_reader.h"
#include "test_support.h"
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sessionscope {
namespace test {

// A helper that creates a writable mapping of the given name and size,
// and allows writing doubles into it. Used by tests to simulate the writer.
class WritableRing {
public:
    explicit WritableRing(const char *name, size_t bytes)
        : name_(name), bytes_(bytes) {
#ifdef _WIN32
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(bytes_), name_);
        if (!mapping_) return;
        base_ = static_cast<volatile double *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bytes_));
#else
        fd_ = shm_open(name_, O_CREAT | O_RDWR, 0600);
        if (fd_ < 0) return;
        if (ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
            close(fd_);
            fd_ = -1;
            return;
        }
        void *p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            return;
        }
        base_ = static_cast<volatile double *>(p);
#endif
    }

    ~WritableRing() {
        if (base_) {
#ifdef _WIN32
            UnmapViewOfFile(const_cast<void *>(static_cast<const volatile void *>(base_)));
            if (mapping_) CloseHandle(mapping_);
#else
            munmap(const_cast<void *>(static_cast<const volatile void *>(base_)), bytes_);
            if (fd_ >= 0) close(fd_);
            shm_unlink(name_);
#endif
        }
    }

    bool valid() const { return base_ != nullptr; }

    void put(std::size_t slot, double value) {
        if (base_) base_[slot] = value;
    }

    double get(std::size_t slot) const {
        return base_ ? base_[slot] : 0.0;
    }

private:
    const char *name_;
    size_t bytes_;
    volatile double *base_ = nullptr;

#ifdef _WIN32
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Generates a unique test mapping name using the process ID.
std::string test_sensor_name() {
    char buf[64];
#ifdef _WIN32
    std::snprintf(buf, sizeof(buf), "SessionScope_test_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
#else
    std::snprintf(buf, sizeof(buf), "/SessionScope_test_%d", getpid());
#endif
    return std::string(buf);
}

std::string test_host_name() {
    char buf[64];
#ifdef _WIN32
    std::snprintf(buf, sizeof(buf), "SessionScope_host_test_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
#else
    std::snprintf(buf, sizeof(buf), "/SessionScope_host_test_%d", getpid());
#endif
    return std::string(buf);
}

}  // namespace test
}  // namespace sessionscope


// ============================================================================
// BarCounter tests (port of BarCounterTest)
// ============================================================================

void test_bar_counter_counts_bars_across_signature_changes() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    // F8's bar starts in quarter notes: 4 bars of 4/4, 4 of 3/4, 4 of 7/8, 4 of 4/4.
    std::vector<std::pair<double, double>> sigs;
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(3, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(7, 8);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);

    std::vector<double> starts = {0.0};
    for (auto [num, den] : sigs) {
        starts.push_back(starts.back() + num * 4.0 / den);
    }

    BarCounter bc;
    std::vector<double> got;
    uint64_t seq = 0;
    const int per_bar = 3;

    for (size_t b = 0; b < 16; ++b) {
        auto [num, den] = sigs[b];
        for (int i = 0; i < per_bar; ++i) {
            got.push_back(bc.measure(starts[b], seq, 1.0, num, den));
            ++seq;
        }
    }

    // One measure per bar, in order (every per_bar-th entry).
    for (size_t i = 0; i < 16; ++i) {
        CHECK_NEAR(got[i * per_bar], static_cast<double>(i), 1e-9);
    }

    // The naive count (qn / current bar length) is wrong from bar 6 on:
    // 4/4 bars then 3/4 ones.
    double naive = std::floor(starts[6] / 3.0);
    CHECK(naive != 6.0);
}

void test_bar_counter_loop_wrap_and_seek_back_reuse_counted_bars() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::vector<std::pair<double, double>> sigs;
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(3, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(7, 8);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);

    std::vector<double> starts = {0.0};
    for (auto [num, den] : sigs) {
        starts.push_back(starts.back() + num * 4.0 / den);
    }

    BarCounter bc;
    uint64_t seq = 0;
    const int per_bar = 3;
    std::vector<double> got;

    // Play through all 16 bars.
    for (size_t b = 0; b < 16; ++b) {
        auto [num, den] = sigs[b];
        for (int i = 0; i < per_bar; ++i) {
            bc.measure(starts[b], seq, 1.0, num, den);
            ++seq;
        }
    }

    // Loop wrap to bar 1 (which is index 1).
    got.clear();
    for (int b : {0, 1}) {
        auto [num, den] = sigs[b];
        for (int i = 0; i < per_bar; ++i) {
            got.push_back(bc.measure(starts[b], seq, 1.0, num, den));
            ++seq;
        }
    }
    CHECK_NEAR(got[0], 0.0, 1e-9);
    CHECK_NEAR(got[per_bar], 1.0, 1e-9);

    // Seek (a sequence gap) into 7/8 region (bar 10 and 11).
    seq += 50;  // sequence gap
    got.clear();
    for (int b : {10, 11}) {
        auto [num, den] = sigs[b];
        for (int i = 0; i < per_bar; ++i) {
            got.push_back(bc.measure(starts[b], seq, 1.0, num, den));
            ++seq;
        }
    }
    CHECK_NEAR(got[0], 10.0, 1e-9);
    CHECK_NEAR(got[per_bar], 11.0, 1e-9);
}

void test_bar_counter_unseen_bar_after_jump_is_guessed_and_not_remembered() {
    using namespace sessionscope;

    std::vector<std::pair<double, double>> sigs;
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(3, 4);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(7, 8);
    for (int i = 0; i < 4; ++i) sigs.emplace_back(4, 4);

    std::vector<double> starts = {0.0};
    for (auto [num, den] : sigs) {
        starts.push_back(starts.back() + num * 4.0 / den);
    }

    BarCounter bc;
    auto [num, den] = sigs[13];
    double m = bc.measure(starts[13], 100, 1.0, num, den);
    double expected = std::floor(starts[13] / 4.0);
    CHECK_NEAR(m, expected, 1e-9);
}

void test_bar_counter_no_bar_start_from_host_gives_nan() {
    using namespace sessionscope;

    BarCounter bc;
    double m = bc.measure(std::numeric_limits<double>::quiet_NaN(), 0, 1.0, 4.0, 4.0);
    CHECK(std::isnan(m));
}


// ============================================================================
// SharedSensor tests (port of SharedSensorTest)
// ============================================================================

void test_shared_sensor_reads_new_records_in_order_and_flags_second_sensor() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name();
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    // Initialize header.
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);        // count
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);       // owner id
    put(6, now_seconds());  // heartbeat
    put(7, 0.0);        // other id

    SharedSensor rd(name.c_str());
    rd.status();  // Initialize the reader; header is already valid
    uint64_t n = 0;

    auto write = [&](int k) {
        double t = now_seconds();
        std::size_t base = HDR + (n % CAP) * REC;
        double qn = k * 0.01;
        // Slot order: t, frames, srate, play_state, play_pos, qn, tempo,
        // ts_num, ts_den, self_us, seq, instance, bar_qn
        std::vector<double> vals = {
            t, 256, 48000, 1, k * 0.005, qn, 120, 4, 4, 1.0,
            static_cast<double>(n), 77.0, std::floor(qn / 4.0) * 4.0
        };
        for (std::size_t j = 0; j < vals.size(); ++j) {
            put(base + j, vals[j]);
        }
        ++n;
        put(2, static_cast<double>(n));
        put(6, t);
    };

    write(0);
    CHECK(rd.poll() == 0);  // history before the first poll is skipped

    for (int k = 1; k <= 5; ++k) write(k);
    CHECK(rd.poll() == 5);

    const auto &seq_col = rd.sensor()["seq"];
    CHECK(seq_col.size() == 5);
    for (int i = 0; i < 5; ++i) {
        CHECK_NEAR(seq_col[i], static_cast<double>(i + 1), 1e-9);
    }

    CHECK_NEAR(rd.sensor()["r_measure"][0], 0.0, 1e-9);  // bar start 0 is the song's first bar
    CHECK(rd.status() == "live");
    CHECK(!rd.second_sensor());

    // Announce a second sensor.
    put(7, 99.0);
    put(8, now_seconds());
    write(6);
    CHECK(rd.second_sensor());
}

void test_shared_sensor_waiting_when_mapping_does_not_exist() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_nonexistent";
    SharedSensor rd(name.c_str());
    CHECK(rd.status() == "waiting");
    CHECK(rd.poll() == 0);
}

void test_shared_sensor_starts_working_after_writer_creates_mapping() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_late";
    // Create the reader BEFORE the writer creates the mapping.
    SharedSensor rd(name.c_str());
    CHECK(rd.status() == "waiting");
    CHECK(rd.poll() == 0);

    // Now create the mapping and write the header.
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    // Reader should now see it.
    CHECK(rd.status() == "live");

    // Write one record.
    std::size_t base = HDR + (0 % CAP) * REC;
    double t = now_seconds();
    std::vector<double> vals = {t, 256, 48000, 1, 0.005, 0.01, 120, 4, 4, 1.0, 0.0, 77.0, 0.0};
    for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
    put(2, 1.0);
    put(6, t);

    CHECK(rd.poll() == 0);  // first poll after header init skips history
    // Write another record
    base = HDR + (1 % CAP) * REC;
    vals[0] = now_seconds();
    vals[5] = 0.02;
    vals[10] = 1.0;
    vals[12] = 0.0;
    for (std::size_t j = 0; j < vals.size(); ++j) put(base + j, vals[j]);
    put(2, 2.0);
    put(6, vals[0]);

    CHECK(rd.poll() == 1);
    CHECK(rd.sensor()["seq"][0] == 1.0);
}

void test_shared_sensor_damaged_header_cap_zero_is_waiting() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_badcap";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 5.0);
    put(3, 0.0);  // cap = 0 (damaged)
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor rd(name.c_str());
    CHECK(rd.poll() == 0);  // damaged header: poll returns 0, doesn't crash
}

void test_shared_sensor_damaged_header_huge_cap_is_waiting() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_hugecap";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 5.0);
    put(3, 1e9);  // huge cap (damaged)
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);

    SharedSensor rd(name.c_str());
    CHECK(rd.poll() == 0);  // damaged header: poll returns 0, doesn't crash
}

// Any of these header values must read as "waiting" with no crash and no bad read.
void test_shared_sensor_other_damaged_headers_are_waiting() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    const double not_a_number = std::numeric_limits<double>::quiet_NaN();
    struct Damage {
        const char *suffix;
        std::size_t slot;
        double value;
    };
    const Damage cases[] = {
        {"_smallrec", 4, 5.0},          // too few slots per record: reading slot 12 would overrun
        {"_nanrec", 4, not_a_number},
        {"_nancap", 3, not_a_number},
        {"_negcap", 3, -1.0},
        {"_onecap", 3, 1.0},            // "the newest capacity - 1 records" would be none, forever
    };
    for (const Damage &damage : cases) {
        std::string name = test_sensor_name() + damage.suffix;
        WritableRing mm(name.c_str(), MAPPING_BYTES);
        CHECK(mm.valid());
        mm.put(0, MAGIC);
        mm.put(1, LAYOUT);
        mm.put(2, 5.0);
        mm.put(3, static_cast<double>(CAP));
        mm.put(4, static_cast<double>(REC));
        mm.put(6, now_seconds());
        mm.put(damage.slot, damage.value);

        SharedSensor rd(name.c_str());
        CHECK(rd.status() == "waiting");
        CHECK(rd.poll() == 0);
    }
}

// A count that is not a plain number must not be converted to an integer.
void test_shared_sensor_unreadable_count_reads_nothing() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    for (double bad_count : {std::numeric_limits<double>::quiet_NaN(), -3.0, 1e300}) {
        std::string name = test_sensor_name() + "_badcount";
        WritableRing mm(name.c_str(), MAPPING_BYTES);
        CHECK(mm.valid());
        mm.put(0, MAGIC);
        mm.put(1, LAYOUT);
        mm.put(2, bad_count);
        mm.put(3, static_cast<double>(CAP));
        mm.put(4, static_cast<double>(REC));
        mm.put(6, now_seconds());

        SharedSensor rd(name.c_str());
        CHECK(rd.poll() == 0);
        CHECK(rd.poll() == 0);
        CHECK(rd.sensor().size() == 0);
    }
}

// A record whose sequence number does not match is half written: drop it, keep the good ones.
void test_shared_sensor_drops_a_torn_record() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_torn";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());
    mm.put(0, MAGIC);
    mm.put(1, LAYOUT);
    mm.put(2, 0.0);
    mm.put(3, static_cast<double>(CAP));
    mm.put(4, static_cast<double>(REC));
    mm.put(6, now_seconds());

    SharedSensor rd(name.c_str());
    CHECK(rd.poll() == 0);   // first look: history is skipped

    const double good_sequence_numbers[] = {0.0, 999.0, 2.0};   // the middle record is torn
    for (std::size_t record = 0; record < 3; ++record) {
        const std::size_t base = HDR + record * REC;
        mm.put(base + 1, 256.0);
        mm.put(base + 2, 48000.0);
        mm.put(base + 10, good_sequence_numbers[record]);
        mm.put(base + 12, std::numeric_limits<double>::quiet_NaN());
    }
    mm.put(2, 3.0);

    CHECK(rd.poll() == 2);
    CHECK(rd.dropped() == 1);
    CHECK(rd.sensor().size() == 2);
    CHECK_NEAR(rd.sensor()["seq"][1], 2.0, 1e-9);
}

void test_shared_sensor_host_process_id_reads_slot_10() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_sensor_name() + "_pid";
    WritableRing mm(name.c_str(), MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, MAGIC);
    put(1, LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(CAP));
    put(4, static_cast<double>(REC));
    put(5, 77.0);
    put(6, now_seconds());
    put(7, 0.0);
    put(10, 12345.0);  // process ID in slot 10

    SharedSensor rd(name.c_str());
    CHECK(rd.status() == "live");
    CHECK_NEAR(rd.host_process_id(), 12345.0, 1e-9);
}


// ============================================================================
// SharedHost tests (port of SharedHostTest)
// ============================================================================

void test_shared_host_reads_realtime_cpu_per_block() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_host_name();
    WritableRing mm(name.c_str(), HOST_MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };

    put(0, HOST_MAGIC);
    put(1, HOST_LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(HOST_CAP));
    put(4, static_cast<double>(HOST_REC));
    put(6, 0.0);  // lost
    put(5, now_seconds());  // heartbeat (QPC)

    SharedHost rd(name.c_str());
    CHECK(rd.poll() == 0);  // first poll only finds the start

    double deadline_us = 256.0 / 48000.0 * 1e6;
    for (int n = 0; n < 3; ++n) {
        double share = (n == 0) ? 0.25 : (n == 1 ? 0.5 : 1.1);
        std::size_t base = HOST_HDR + n * HOST_REC;
        put(base + 0, 10.0 + n * 0.005);       // t
        put(base + 1, share * deadline_us);    // dur_us
        put(base + 2, 256.0);                  // frames
        put(base + 3, 48000.0);                // srate
        put(2, static_cast<double>(n + 1));    // count
    }
    put(5, now_seconds());  // update heartbeat

    CHECK(rd.status() == "live");
    CHECK(rd.poll() == 3);

    auto recent = rd.recent(1.0);
    CHECK(recent.size() == 3);
    CHECK_NEAR(recent[0], 1.1, 1e-6);
    CHECK_NEAR(recent[1], 0.5, 1e-6);
    CHECK_NEAR(recent[2], 0.25, 1e-6);
}

void test_shared_host_waiting_when_mapping_does_not_exist() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_host_name() + "_nonexistent";
    SharedHost rd(name.c_str());
    CHECK(rd.status() == "waiting");
    CHECK(rd.poll() == 0);
}

void test_shared_host_damaged_header_cap_zero_is_waiting() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_host_name() + "_badcap";
    WritableRing mm(name.c_str(), HOST_MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, HOST_MAGIC);
    put(1, HOST_LAYOUT);
    put(2, 5.0);
    put(3, 0.0);  // cap = 0 (damaged)
    put(4, static_cast<double>(HOST_REC));
    put(5, now_seconds());
    put(6, 0.0);
    put(7, 0.0);

    SharedHost rd(name.c_str());
    CHECK(rd.poll() == 0);  // damaged header: poll returns 0, doesn't crash
}

void test_shared_host_starts_working_after_writer_creates_mapping() {
    using namespace sessionscope;
    using namespace sessionscope::test;

    std::string name = test_host_name() + "_late";
    SharedHost rd(name.c_str());
    CHECK(rd.status() == "waiting");
    CHECK(rd.poll() == 0);

    WritableRing mm(name.c_str(), HOST_MAPPING_BYTES);
    CHECK(mm.valid());

    auto put = [&](std::size_t slot, double v) { mm.put(slot, v); };
    put(0, HOST_MAGIC);
    put(1, HOST_LAYOUT);
    put(2, 0.0);
    put(3, static_cast<double>(HOST_CAP));
    put(4, static_cast<double>(HOST_REC));
    put(5, now_seconds());
    put(6, 0.0);
    put(7, 0.0);

    CHECK(rd.status() == "live");

    // Write some blocks
    double deadline_us = 256.0 / 48000.0 * 1e6;
    for (int n = 0; n < 2; ++n) {
        std::size_t base = HOST_HDR + n * HOST_REC;
        put(base + 0, 10.0 + n * 0.005);
        put(base + 1, 0.5 * deadline_us);
        put(base + 2, 256.0);
        put(base + 3, 48000.0);
        put(2, static_cast<double>(n + 1));
    }
    put(5, now_seconds());

    CHECK(rd.poll() == 0);  // first poll after header init skips
    // Write more
    for (int n = 2; n < 4; ++n) {
        std::size_t base = HOST_HDR + n * HOST_REC;
        put(base + 0, 10.0 + n * 0.005);
        put(base + 1, 0.5 * deadline_us);
        put(base + 2, 256.0);
        put(base + 3, 48000.0);
        put(2, static_cast<double>(n + 1));
    }
    put(5, now_seconds());

    CHECK(rd.poll() == 2);
}


// ============================================================================
// Main
// ============================================================================

int main() {
    // BarCounter tests
    test_bar_counter_counts_bars_across_signature_changes();
    test_bar_counter_loop_wrap_and_seek_back_reuse_counted_bars();
    test_bar_counter_unseen_bar_after_jump_is_guessed_and_not_remembered();
    test_bar_counter_no_bar_start_from_host_gives_nan();

    // SharedSensor tests
    test_shared_sensor_reads_new_records_in_order_and_flags_second_sensor();
    test_shared_sensor_waiting_when_mapping_does_not_exist();
    test_shared_sensor_starts_working_after_writer_creates_mapping();
    test_shared_sensor_damaged_header_cap_zero_is_waiting();
    test_shared_sensor_damaged_header_huge_cap_is_waiting();
    test_shared_sensor_other_damaged_headers_are_waiting();
    test_shared_sensor_unreadable_count_reads_nothing();
    test_shared_sensor_drops_a_torn_record();
    test_shared_sensor_host_process_id_reads_slot_10();

    // SharedHost tests - simple ones first
    test_shared_host_waiting_when_mapping_does_not_exist();
    test_shared_host_damaged_header_cap_zero_is_waiting();
    test_shared_host_starts_working_after_writer_creates_mapping();
    test_shared_host_reads_realtime_cpu_per_block();

    return finish_tests();
}
