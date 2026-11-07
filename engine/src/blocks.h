#ifndef SESSIONSCOPE_BLOCKS_H
#define SESSIONSCOPE_BLOCKS_H

// Per-block quantities derived from the sensor rows, built incrementally so live mode can
// reuse them.
//
// Definitions (see docs/metrics.md):
//   deadline D        frames / srate of a block
//   interval          t[k] - t[k-1] between consecutive sensor callbacks
//   lateness          interval - D[k-1]: how much later than one block after the previous
//                     callback this one began. Only computed between records known to be
//                     consecutive: never across dropped records, ring resets or stops.
//   workload share    the Workload JSFX's known cost in this block / D (ground truth, fixtures only)
//
// A pass is one continuous run of playback. It ends when playback stops or the position
// jumps (seek or loop wrap), so repeated bars are never stitched together.

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace sessionscope {

class Table;

constexpr double FORWARD_JUMP = 4.5;     // play rate tops out at 4x: moving faster than 4.5 blocks per block is a seek
constexpr double BACK_TOLERANCE = 0.002; // seconds; any backward move larger than rounding is a loop wrap or seek

// Playing or recording: 1 playing, 5 recording (both have bit 1); 0 stopped, 2 paused, 6 record-paused.
// NaN is not playing.
inline bool is_playing(double state) {
    return !std::isnan(state) && (static_cast<int>(state) & 1) == 1;
}

// Bar from quarter notes and the current signature: wrong after a signature change.
inline int naive_bar(double qn, double ts_num, double ts_den) {
    if (std::isnan(qn) || ts_num == 0 || ts_den == 0 || std::isnan(ts_num) || std::isnan(ts_den)) {
        return 0;
    }
    double qn_per_bar = ts_num * 4.0 / ts_den; // 4/4 -> 4, 3/4 -> 3, 7/8 -> 3.5
    // The epsilon keeps 7.9999999 in bar 2. Python uses floor division via //.
    return static_cast<int>(std::floor(qn / qn_per_bar + 1e-9)) + 1;
}

struct Pass {
    int id = 0;                    // 1-based pass identifier
    std::size_t first = 0;         // index of the pass's first block
    std::string kind;              // how it began: start, jump_back (loop or seek back), jump (seek forward), gap
    std::ptrdiff_t last = -1;      // index of its last block; -1 while the pass is still going
    int first_bar = 0;
    int last_bar = 0;

    // Number of blocks in this pass (0 if not closed yet)
    std::size_t blocks() const {
        return last >= 0 ? static_cast<std::size_t>(last - static_cast<std::ptrdiff_t>(first) + 1) : 0;
    }
};

class Blocks {
public:
    // Construct from a sensor table. Does not process rows until update() is called.
    explicit Blocks(const Table &sensor);

    // Process only rows added since the last update(). Returns *this for chaining.
    Blocks &update();

    // Close the open pass at the last block (call once the trace is complete).
    Blocks &finish();

    // Return pointer to pass by 1-based id, or nullptr if not found.
    const Pass *pass_by_id(int pid) const;

    // Number of rows processed so far.
    std::size_t size() const { return done_; }

    // Forgets the first `rows` processed rows, after the same rows were dropped from the sensor
    // table (Table::drop_front), so row numbers keep matching it. Keep at least one row: the next
    // block is compared with the one before it. Passes that began earlier are kept, with `first`
    // moved to row 0. Counters (missing, resets) are unaffected.
    void drop_front(std::size_t rows);

    // Public read-only vectors with the Python names, indexed by sensor row.
    const std::vector<int> &pass_id() const { return pass_id_; }
    const std::vector<int> &bar() const { return bar_; }
    const std::vector<double> &d_ms() const { return d_ms_; }
    const std::vector<double> &interval_ms() const { return interval_ms_; }
    const std::vector<double> &lateness_ms() const { return lateness_ms_; }
    const std::vector<double> &workload_share() const { return workload_share_; }
    const std::vector<Pass> &passes() const { return passes_; }
    std::size_t missing() const { return missing_; }
    std::size_t resets() const { return resets_; }

private:
    const Table &src_;
    std::size_t done_ = 0;              // rows processed so far
    bool playing_ = false;              // was the previous block playing?

    std::vector<int> pass_id_;          // 0 = not playing, else the pass the block belongs to
    std::vector<int> bar_;              // 1-based bar, 0 = unknown
    std::vector<double> d_ms_;          // deadline D in ms
    std::vector<double> interval_ms_;   // NaN when the previous record is not known to be the previous block
    std::vector<double> lateness_ms_;   // interval - D[prev] in ms
    std::vector<double> workload_share_;// workload_us / D
    std::vector<Pass> passes_;
    std::size_t missing_ = 0;           // records the recorder never saw (sequence gaps)
    std::size_t resets_ = 0;            // sensor restarted its ring, or another instance took over
};

// A complete trace in one call.
inline Blocks build_blocks(const Table &sensor) {
    Blocks blocks(sensor);
    blocks.update();
    blocks.finish();
    return blocks;
}

class HostBlocks {
public:
    // Construct from host and transport tables. Does not process rows until update() is called.
    HostBlocks(const Table &host, const Table &transport);

    // Process only rows added since the last update(). Returns *this for chaining.
    HostBlocks &update();

    // Number of rows processed so far.
    std::size_t size() const { return done_; }

    // Public read-only vectors.
    const std::vector<int> &pass_id() const { return pass_id_; }
    const std::vector<int> &bar() const { return bar_; }
    const std::vector<double> &load() const { return load_; }

private:
    const Table &src_;
    const Table &tr_;
    std::size_t done_ = 0;

    std::vector<int> pass_id_;
    std::vector<int> bar_;
    std::vector<double> load_;          // time the host spent on the block / the block's deadline
};

} // namespace sessionscope

#endif // SESSIONSCOPE_BLOCKS_H
