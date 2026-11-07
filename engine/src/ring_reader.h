#ifndef SESSIONSCOPE_RING_READER_H
#define SESSIONSCOPE_RING_READER_H

// Follows a running sensor through shared memory, on Windows, macOS and Linux. Everything here is
// read-only and never creates a mapping, so nothing the engine does can disturb the sensor or the
// DAW. A header that looks wrong counts as "waiting": bad data must never cause a bad read.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sensor_ring.h"
#include "trace.h"

namespace sessionscope {

// The optional REAPER extension's ring (hosts/reaper/extension): the host's realtime CPU per block.
#ifdef _WIN32
constexpr const char *HOST_MAPPING_NAME = "SessionScope_host_v1";
#else
constexpr const char *HOST_MAPPING_NAME = "/SessionScope_host_v1";
#endif
constexpr double HOST_MAGIC = 53530002;
constexpr double HOST_LAYOUT = 1;
constexpr std::size_t HOST_HDR = 8;
constexpr std::size_t HOST_REC = 4;
constexpr std::uint64_t HOST_CAP = 16384;
constexpr std::size_t HOST_MAPPING_BYTES = (HOST_HDR + HOST_CAP * HOST_REC) * sizeof(double);

constexpr double STALE_S = 1.0;   // an owner that has not written for this long is gone


// A read-only view of a named mapping of doubles. Only the writer may create a mapping: macOS
// allows sizing one only once, and a reader must have no way to affect the sensor.
class MappedRing {
public:
    MappedRing(const char *name, std::size_t bytes) : name_(name), bytes_(bytes) {}
    ~MappedRing();
    MappedRing(const MappedRing &) = delete;
    MappedRing &operator=(const MappedRing &) = delete;

    // True once the mapping exists. Until then it tries again on every call: a sensor that has not
    // started yet is the normal state, not an error.
    bool open();
    bool is_open() const { return base_ != nullptr; }

    double slot(std::size_t index) const { return base_[index]; }

private:
    const char *name_;
    std::size_t bytes_;
    const volatile double *base_ = nullptr;
};


// Bar numbers from the host's bar start (VST3 barPositionMusic), right across signature changes.
//
// Quarter notes plus the current signature give the wrong bar after a time-signature change. The
// host's bar start does not tell the bar number either, only where the bar began, so bars are
// counted: during continuous playback each new bar start is the previous bar + 1. Every bar learned
// that way is remembered by its start, so after a loop wrap or a seek back to a bar already played
// the number is known again. A jump to a bar never played falls back to the naive count from the
// current signature (correct when no signature changed before it), and that guess is not remembered.
class BarCounter {
public:
    static constexpr double EPS = 1e-6;   // quarter notes; bar starts closer than this are the same bar

    // The 0-based measure for one record, or NaN if the host gives no bar start.
    double measure(double bar_qn, double seq, double instance, double ts_num, double ts_den);

private:
    std::unordered_map<std::int64_t, double> known_;   // rounded bar start -> measure counted from playback
    bool has_previous_record_ = false;
    double previous_seq_ = 0;
    double previous_instance_ = 0;
    bool has_previous_bar_ = false;
    std::int64_t previous_key_ = 0;
    double previous_measure_ = std::numeric_limits<double>::quiet_NaN();
    bool previous_sure_ = false;                       // was previous_measure_ counted, or guessed?
};


// Drains the sensor ring into a Table with the same columns as a trace's sensor.csv, so everything
// that works on traces also works live. Call poll() often.
class SharedSensor {
public:
    explicit SharedSensor(const char *mapping_name = MAPPING_NAME);

    // "waiting" (no valid ring yet), "live", or "stale" (the plug-in stopped writing).
    std::string_view status();

    // True while a second plug-in instance announces itself next to the owner.
    bool second_sensor();

    // Reads new records into the table; returns how many.
    std::size_t poll();

    std::uint64_t dropped() const { return dropped_; }   // records lost to overwriting or torn reads

    // The process of the host that owns the ring, for reading its CPU use. 0 if unknown.
    double host_process_id() const { return ring_.is_open() ? ring_.slot(10) : 0.0; }

    const Table &sensor() const { return sensor_; }

    // Forgets the oldest rows of the table (Table::drop_front). Reading the ring is unaffected.
    void drop_front(std::size_t rows) { sensor_.drop_front(rows); }

private:
    bool header_is_valid() const;

    Table sensor_;
    MappedRing ring_;
    BarCounter bars_;
    bool has_next_ = false;             // false until the first poll sees the ring
    std::uint64_t next_ = 0;            // next record number to read
    std::uint64_t dropped_ = 0;
    std::vector<double> copied_records_;   // reused between polls
    std::vector<double> table_row_;        // reused between rows
};


// Reads the optional REAPER extension's ring: realtime CPU per block. Keeps only the last few seconds.
class SharedHost {
public:
    static constexpr std::size_t KEEP = 8192;   // over 40 s at 48 kHz / 256, plenty for a 1 s readout

    struct HostBlock {
        double t;      // on the sensor's clock
        double load;   // the host's time on the block / the block's deadline
    };

    explicit SharedHost(const char *mapping_name = HOST_MAPPING_NAME);

    // "waiting" (extension not loaded), "live", or "stale" (the host stopped or quit).
    std::string_view status();

    // Reads new blocks; returns how many.
    std::size_t poll();

    std::size_t lost() const { return lost_; }                      // blocks the extension had to drop
    const std::vector<HostBlock> &fresh() const { return fresh_; }  // what the last poll added
    std::vector<double> recent(double seconds = 1.0) const;         // loads of the last `seconds`, newest first
    const std::deque<HostBlock> &blocks() const { return blocks_; } // for HudEngine bar chart placement

private:
    bool header_is_valid() const;

    MappedRing ring_;
    std::deque<HostBlock> blocks_;
    std::vector<HostBlock> fresh_;
    bool has_next_ = false;
    std::uint64_t next_ = 0;
    std::size_t lost_ = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_RING_READER_H
