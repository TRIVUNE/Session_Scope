// Derives per-block values and passes from the sensor rows. The header explains the definitions.

#include "blocks.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "trace.h"

namespace sessionscope {

Blocks::Blocks(const Table &sensor) : src_(sensor) {}

Blocks &Blocks::update() {
    const std::size_t n = src_.size();
    if (done_ >= n) return *this;

    // Take a const reference to each input column once, before the loop.
    const auto &seq       = src_["seq"];
    const auto &instance  = src_["instance"];
    const auto &t         = src_["t"];
    const auto &frames    = src_["frames"];
    const auto &srate     = src_["srate"];
    const auto &play_state= src_["play_state"];
    const auto &play_pos  = src_["play_pos"];
    const auto &qn        = src_["qn"];
    const auto &ts_num    = src_["ts_num"];
    const auto &ts_den    = src_["ts_den"];
    const auto &r_measure = src_["r_measure"];
    const auto &workload_us = src_["workload_us"];

    const std::size_t new_rows = n - done_;
    pass_id_.reserve(pass_id_.size() + new_rows);
    bar_.reserve(bar_.size() + new_rows);
    d_ms_.reserve(d_ms_.size() + new_rows);
    interval_ms_.reserve(interval_ms_.size() + new_rows);
    lateness_ms_.reserve(lateness_ms_.size() + new_rows);
    workload_share_.reserve(workload_share_.size() + new_rows);

    for (std::size_t i = done_; i < n; ++i) {
        // Deadline D in ms: frames / srate * 1000, NaN when srate <= 0.
        const double srate_i = srate[i];
        const double d = (srate_i > 0) ? frames[i] / srate_i : std::numeric_limits<double>::quiet_NaN();
        d_ms_.push_back(d * 1000.0);

        // Is the previous record really the previous block?
        // Same sensor, next sequence number, and not separated by a long silence.
        bool contiguous = false;
        if (i > 0) {
            const std::size_t p = i - 1;
            if (seq[i] < seq[p] || instance[i] != instance[p]) {
                ++resets_;
            } else if (seq[i] > seq[p] + 1) {
                missing_ += static_cast<std::size_t>(seq[i] - seq[p] - 1);
            } else {
                const double gap = t[i] - t[p];
                // Python: 0 < gap < max(0.5, 20 * self.d_ms[p] / 1000)
                // d_ms[p] is in ms, so / 1000 converts to seconds.
                const double threshold = std::max(0.5, 20.0 * d_ms_[p] / 1000.0);
                contiguous = (0.0 < gap) && (gap < threshold);
            }
        }

        if (contiguous) {
            const double interval = t[i] - t[i - 1];
            interval_ms_.push_back(interval * 1000.0);
            // Lateness uses d_ms of the PREVIOUS block.
            lateness_ms_.push_back((interval * 1000.0) - d_ms_[i - 1]);
        } else {
            interval_ms_.push_back(std::numeric_limits<double>::quiet_NaN());
            lateness_ms_.push_back(std::numeric_limits<double>::quiet_NaN());
        }

        // Bar: the host's own bar for this block when the source gave one, otherwise the sensor's guess.
        int b = 0;
        const double r_measure_i = r_measure[i];
        if (!std::isnan(r_measure_i)) {
            // Python: int(r_measure[i]) + 1 (truncates toward zero)
            b = static_cast<int>(r_measure_i) + 1;
        } else {
            b = naive_bar(qn[i], ts_num[i], ts_den[i]);
        }
        bar_.push_back(b);

        // Workload share: workload_us / (d * 1e6) if d > 0 and workload_us is not NaN, else NaN.
        // Python: wl[i] / (d * 1e6) if not math.isnan(wl[i]) and d > 0 else NAN
        const double wl_i = workload_us[i];
        if (!std::isnan(wl_i) && d > 0) {
            workload_share_.push_back(wl_i / (d * 1e6));
        } else {
            workload_share_.push_back(std::numeric_limits<double>::quiet_NaN());
        }

        // Pass detection: a new pass starts when playback starts, or the position jumps.
        const bool playing = is_playing(play_state[i]);
        std::string kind;

        if (playing) {
            if (!playing_) {
                kind = "start";
            } else if (i > 0) {
                // Expected advance: one block per record, or more if records were lost in between.
                // Python: steps = seq[i] - seq[i - 1] if seq[i] > seq[i - 1] else 1
                const double steps = (seq[i] > seq[i - 1]) ? (seq[i] - seq[i - 1]) : 1.0;
                const double dpos = play_pos[i] - play_pos[i - 1];
                // Python: expected = steps * self.d_ms[i - 1] / 1000
                const double expected = steps * d_ms_[i - 1] / 1000.0;

                if (dpos < -BACK_TOLERANCE) {
                    kind = "jump_back";
                } else if (dpos > FORWARD_JUMP * expected + BACK_TOLERANCE) {
                    kind = "jump";
                } else if (!contiguous && (t[i] - t[i - 1]) >= 0.5) {
                    kind = "gap";
                }
            }

            if (!kind.empty()) {
                // Close the previous pass if it's still open.
                if (!passes_.empty() && passes_.back().last < 0) {
                    passes_.back().last = static_cast<std::ptrdiff_t>(i - 1);
                }
                passes_.emplace_back();
                Pass &new_pass = passes_.back();
                new_pass.id = static_cast<int>(passes_.size());
                new_pass.first = i;
                new_pass.kind = std::move(kind);
                new_pass.first_bar = b;
            }

            Pass &cur = passes_.back();
            cur.last_bar = b;
            pass_id_.push_back(cur.id);
        } else {
            if (playing_ && !passes_.empty() && passes_.back().last < 0) {
                passes_.back().last = static_cast<std::ptrdiff_t>(i - 1);
            }
            pass_id_.push_back(0);
        }

        playing_ = playing;
    }

    done_ = n;
    return *this;
}

Blocks &Blocks::finish() {
    if (!passes_.empty() && passes_.back().last < 0) {
        passes_.back().last = static_cast<std::ptrdiff_t>(done_ - 1);
    }
    return *this;
}

void Blocks::drop_front(std::size_t rows) {
    rows = std::min(rows, done_);
    const auto drop = [rows](auto &column) { column.erase(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(rows)); };
    drop(pass_id_);
    drop(bar_);
    drop(d_ms_);
    drop(interval_ms_);
    drop(lateness_ms_);
    drop(workload_share_);
    done_ -= rows;
    for (Pass &pass : passes_) {
        pass.first = pass.first > rows ? pass.first - rows : 0;
        if (pass.last >= 0) pass.last = std::max<std::ptrdiff_t>(0, pass.last - static_cast<std::ptrdiff_t>(rows));
    }
}

const Pass *Blocks::pass_by_id(int pid) const {
    if (pid > 0 && static_cast<std::size_t>(pid) <= passes_.size()) {
        return &passes_[pid - 1];
    }
    return nullptr;
}

HostBlocks::HostBlocks(const Table &host, const Table &transport) : src_(host), tr_(transport) {}

HostBlocks &HostBlocks::update() {
    const std::size_t n = src_.size();
    if (done_ >= n) return *this;

    // Take a const reference to each input column once, before the loop.
    const auto &h_t        = src_["t"];
    const auto &h_frames   = src_["frames"];
    const auto &h_srate    = src_["srate"];
    const auto &h_dur_us   = src_["dur_us"];

    const auto &tt         = tr_["t"];
    const auto &tr_pass    = tr_["pass"];
    const auto &tr_play_state = tr_["play_state"];
    const auto &tr_measure = tr_["measure"];
    const auto &tr_ts_num  = tr_["ts_num"];
    const auto &tr_ts_den  = tr_["ts_den"];
    const auto &tr_tempo   = tr_["tempo"];
    const auto &tr_rate    = tr_["rate"];
    const auto &tr_beat    = tr_["beat"];

    const std::size_t new_rows = n - done_;
    pass_id_.reserve(pass_id_.size() + new_rows);
    bar_.reserve(bar_.size() + new_rows);
    load_.reserve(load_.size() + new_rows);

    for (std::size_t i = done_; i < n; ++i) {
        const double t_i = h_t[i];
        const double srate_i = h_srate[i];
        const double d = (srate_i > 0) ? h_frames[i] / srate_i : std::numeric_limits<double>::quiet_NaN();
        load_.push_back((d > 0) ? h_dur_us[i] / (d * 1e6) : std::numeric_limits<double>::quiet_NaN());

        // The latest transport sample at or before this block (binary search: samples are in time order).
        // Python: bisect.bisect_right(tt, t) - 1
        const auto after = std::upper_bound(tt.begin(), tt.end(), t_i);
        const bool has_sample = after != tt.begin();
        const std::size_t k = has_sample ? static_cast<std::size_t>(after - tt.begin()) - 1 : 0;

        int pid = 0;
        int bar = 0;

        // A blank measure leaves the block unplaced: missing data never becomes a bar number.
        if (has_sample && t_i - tt[k] < 0.2 && is_playing(tr_play_state[k]) && tr_pass[k] > 0
            && !std::isnan(tr_measure[k])) {
            pid = static_cast<int>(tr_pass[k]);
            bar = static_cast<int>(tr_measure[k]) + 1;

            const double num = tr_ts_num[k];
            const double den = tr_ts_den[k];
            const double tempo = tr_tempo[k];
            const double rate = std::isnan(tr_rate[k]) ? 1.0 : tr_rate[k];

            if (num > 0 && den > 0 && tempo > 0) {
                // Move forward from the sample by the elapsed time, in beats of the signature.
                const double beats = tr_beat[k] + (t_i - tt[k]) * rate * tempo / 60.0 * den / 4.0;
                const int crossed = std::isnan(beats) ? 0 : static_cast<int>(std::floor(beats / num));
                const bool pass_ended = k + 1 < tt.size() && tr_pass[k + 1] != pid;

                if (crossed != 0 && pass_ended) {
                    // The pass ended (stop or loop wrap) before the next sample: the block was in its
                    // last bar or already in the next pass. Leave it unplaced, never invent a bar.
                    pid = 0;
                    bar = 0;
                } else {
                    bar += crossed;
                }
            }
        }

        pass_id_.push_back(pid);
        bar_.push_back(bar);
    }

    done_ = n;
    return *this;
}

} // namespace sessionscope
