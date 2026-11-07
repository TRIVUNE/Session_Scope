// Shared-memory reader implementation. The ring layout and the writer's protocol are documented in
// native/sensor_core/sensor_ring.h.

#include "ring_reader.h"

#include <atomic>

namespace sessionscope {

namespace {

const double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

// A record count from shared memory. Converting NaN or a negative double to an integer is undefined
// behaviour, so anything that is not a plain count gives false.
bool read_count(double slot_value, std::uint64_t &count) {
    if (!(slot_value >= 0 && slot_value < 9e15)) return false;
    count = static_cast<std::uint64_t>(slot_value);
    return true;
}

}  // namespace

// ------------------------------------------------------------------------------------ MappedRing

#ifdef _WIN32

bool MappedRing::open() {
    if (base_) return true;
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name_);
    if (!mapping) return false;
    base_ = static_cast<const volatile double *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes_));
    CloseHandle(mapping);   // the view keeps the mapping alive
    return base_ != nullptr;
}

MappedRing::~MappedRing() {
    if (base_) UnmapViewOfFile(const_cast<const double *>(base_));
}

#else

bool MappedRing::open() {
    if (base_) return true;
    const int file = shm_open(name_, O_RDONLY, 0);
    if (file < 0) return false;

    struct stat info;
    const bool big_enough = fstat(file, &info) == 0 && static_cast<std::size_t>(info.st_size) >= bytes_;
    void *view = big_enough ? mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, file, 0) : MAP_FAILED;
    close(file);   // the view keeps the mapping alive

    if (view != MAP_FAILED) base_ = static_cast<const volatile double *>(view);
    return base_ != nullptr;
}

MappedRing::~MappedRing() {
    if (base_) munmap(const_cast<double *>(base_), bytes_);
}

#endif

// ------------------------------------------------------------------------------------ BarCounter

double BarCounter::measure(double bar_qn, double seq, double instance, double ts_num, double ts_den) {
    // The previous record is the previous block: same plug-in instance, next sequence number.
    const bool contiguous = has_previous_record_ && instance == previous_instance_ && seq == previous_seq_ + 1;
    has_previous_record_ = true;
    previous_seq_ = seq;
    previous_instance_ = instance;

    if (!std::isfinite(bar_qn)) {   // a host without bar start
        has_previous_bar_ = false;
        previous_measure_ = NOT_A_NUMBER;
        previous_sure_ = false;
        return NOT_A_NUMBER;
    }

    const std::int64_t key = static_cast<std::int64_t>(std::nearbyint(bar_qn / EPS));   // an exact key for this bar start
    const bool can_continue = contiguous && has_previous_bar_;

    double measure = NOT_A_NUMBER;
    bool sure = false;
    if (const auto found = known_.find(key); found != known_.end()) {   // a bar already counted
        measure = found->second;
        sure = true;
    } else if (can_continue && key == previous_key_) {                  // still in the previous block's bar
        measure = previous_measure_;
        sure = previous_sure_;
    } else if (can_continue && key > previous_key_ && !std::isnan(previous_measure_)) {   // the next bar began here
        measure = previous_measure_ + 1;
        sure = previous_sure_;
    } else if (key == 0) {                                              // the song's first bar
        measure = 0;
        sure = true;
    } else {                                                            // a jump to a bar never played: guess
        const double qn_per_bar = (ts_num > 0 && ts_den > 0) ? ts_num * 4.0 / ts_den : 4.0;
        measure = std::floor(bar_qn / qn_per_bar + 1e-9);
    }

    if (sure) known_[key] = measure;   // remember counted bars only
    has_previous_bar_ = true;
    previous_key_ = key;
    previous_measure_ = measure;
    previous_sure_ = sure;
    return measure;
}

// ---------------------------------------------------------------------------------- SharedSensor

SharedSensor::SharedSensor(const char *mapping_name)
    : sensor_(SENSOR_COLUMNS), ring_(mapping_name, MAPPING_BYTES), table_row_(SENSOR_COLUMNS.size()) {}

bool SharedSensor::header_is_valid() const {
    const double capacity = ring_.slot(3);
    return ring_.slot(0) == MAGIC && ring_.slot(1) == LAYOUT && capacity >= 2 &&
           capacity <= static_cast<double>(CAP) && ring_.slot(4) == REC;
}

std::string_view SharedSensor::status() {
    if (!ring_.open() || !header_is_valid()) return "waiting";
    return now_seconds() - ring_.slot(6) < STALE_S ? "live" : "stale";
}

bool SharedSensor::second_sensor() {
    if (status() == "waiting") return false;
    const double now = now_seconds();
    const double other_id = ring_.slot(7);
    return other_id != 0 && other_id != ring_.slot(5) && now - ring_.slot(8) < 0.5 && now - ring_.slot(6) < 0.5;
}

std::size_t SharedSensor::poll() {
    std::uint64_t count = 0;
    if (status() == "waiting" || !read_count(ring_.slot(2), count)) return 0;
    const std::uint64_t capacity = static_cast<std::uint64_t>(ring_.slot(3));

    if (!has_next_) {               // skip history from before the engine started
        has_next_ = true;
        next_ = count;
        return 0;
    }
    if (count < next_) next_ = 0;   // the plug-in restarted its ring

    // While the count is n + capacity the writer may already be reusing record n's slot, so only
    // the newest capacity - 1 records are safe to read.
    if (count - next_ > capacity - 1) {
        const std::uint64_t oldest_safe = count - (capacity - 1);
        dropped_ += oldest_safe - next_;
        next_ = oldest_safe;
    }

    // Pairs with the writer's release fence: what we copy next is at least as new as `count`.
    std::atomic_thread_fence(std::memory_order_acquire);

    const std::uint64_t first = next_;
    const std::size_t rows = static_cast<std::size_t>(count - first);
    copied_records_.resize(rows * REC);
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t base = HDR + static_cast<std::size_t>((first + row) % capacity) * REC;
        for (std::size_t slot = 0; slot < REC; ++slot) copied_records_[row * REC + slot] = ring_.slot(base + slot);
    }

    // The writer never waits for us, so check afterwards which copies it may have overwritten.
    std::atomic_thread_fence(std::memory_order_acquire);
    std::uint64_t count_after = count;
    read_count(ring_.slot(2), count_after);

    std::size_t added = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        const double *record = copied_records_.data() + row * REC;
        const std::uint64_t record_number = first + row;

        const bool overwritten = count_after >= record_number + capacity;
        const bool torn = record[10] != static_cast<double>(record_number);
        if (overwritten || torn) {
            ++dropped_;
            continue;
        }

        // The same columns and order as sensor.csv. The plug-in knows the host's bar start but not
        // a bar number or a workload.
        table_row_ = {record[10], record[11], record[0], record[1], record[2], record[3], record[4], record[5],
                      record[6],  record[7],  record[8], record[9],
                      bars_.measure(record[12], record[10], record[11], record[7], record[8]),
                      NOT_A_NUMBER, NOT_A_NUMBER, NOT_A_NUMBER};
        sensor_.append_row(table_row_);
        ++added;
    }
    next_ = count;
    return added;
}

// ------------------------------------------------------------------------------------ SharedHost

SharedHost::SharedHost(const char *mapping_name) : ring_(mapping_name, HOST_MAPPING_BYTES) {}

bool SharedHost::header_is_valid() const {
    const double capacity = ring_.slot(3);
    return ring_.slot(0) == HOST_MAGIC && ring_.slot(1) == HOST_LAYOUT && capacity >= 2 &&
           capacity <= static_cast<double>(HOST_CAP) && ring_.slot(4) == HOST_REC;
}

std::string_view SharedHost::status() {
    if (!ring_.open() || !header_is_valid()) return "waiting";
    return now_seconds() - ring_.slot(5) < STALE_S ? "live" : "stale";
}

std::size_t SharedHost::poll() {
    fresh_.clear();
    std::uint64_t count = 0;
    if (status() == "waiting" || !read_count(ring_.slot(2), count)) return 0;
    const std::uint64_t capacity = static_cast<std::uint64_t>(ring_.slot(3));

    std::uint64_t lost = 0;
    if (read_count(ring_.slot(6), lost)) lost_ = static_cast<std::size_t>(lost);

    if (!has_next_ || count < next_) {   // first look, or the host restarted the extension
        has_next_ = true;
        next_ = count;
        return 0;
    }
    if (count - next_ > capacity - 1) next_ = count - (capacity - 1);   // a stalled engine skips what was overwritten

    // The extension stamps blocks on the host's clock and publishes both clocks read together, so
    // convert to the sensor's clock rather than assume the two agree.
    const double host_clock = ring_.slot(7);
    const double to_sensor_clock = host_clock > 0 ? ring_.slot(5) - host_clock : 0.0;

    std::atomic_thread_fence(std::memory_order_acquire);   // pairs with the writer's release fence
    for (std::uint64_t record = next_; record < count; ++record) {
        const std::size_t base = HOST_HDR + static_cast<std::size_t>(record % capacity) * HOST_REC;
        const double frames = ring_.slot(base + 2);
        const double sample_rate = ring_.slot(base + 3);
        if (frames > 0 && sample_rate > 0) {
            const double deadline_us = frames / sample_rate * 1e6;
            fresh_.push_back({ring_.slot(base) + to_sensor_clock, ring_.slot(base + 1) / deadline_us});
        }
    }
    next_ = count;

    blocks_.insert(blocks_.end(), fresh_.begin(), fresh_.end());
    while (blocks_.size() > KEEP) blocks_.pop_front();
    return fresh_.size();
}

std::vector<double> SharedHost::recent(double seconds) const {
    std::vector<double> loads;
    if (blocks_.empty()) return loads;
    const double newest = blocks_.back().t;
    for (auto block = blocks_.rbegin(); block != blocks_.rend() && newest - block->t <= seconds; ++block) {
        loads.push_back(block->load);
    }
    return loads;
}

}  // namespace sessionscope
