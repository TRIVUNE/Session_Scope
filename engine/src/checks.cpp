// The spike checks; checks.h says what each one asks.

#include "checks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

#include "text_format.h"

namespace sessionscope {

const std::string DUMMY_NOTE = ("Recorded on REAPER's dummy audio device: its callbacks follow a timer and it slows down instead of"
                                " dropping out, so lateness and xruns here say nothing about a real interface's deadline.");

namespace {

// Helper to get a column from a Table by name, returning a const reference.
// Throws if column doesn't exist (but we check first in the calling code).
const std::vector<double> &get_col(const Table &table, std::string_view name) {
    return table[name];
}

// Helper to check if a string contains a substring (case-insensitive).
bool contains_ci(std::string_view haystack, std::string_view needle) {
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

// Parses meta.json and returns the JsonValue, or an empty object on failure.
JsonValue parse_meta(const TraceReader &trace) {
    auto parsed = parse_json(trace.meta_json());
    if (parsed) return *parsed;
    return JsonValue(std::vector<std::pair<std::string, JsonValue>>{});  // empty object
}

}  // namespace

bool dummy_device(const TraceReader &trace) {
    JsonValue meta = parse_meta(trace);
    JsonValue device = meta["device"];
    JsonValue mode = device["mode"];
    std::string mode_str = mode.text();
    return contains_ci(mode_str, "dummy");
}

std::vector<double> xrun_times(const TraceReader &trace, std::string_view kind) {
    std::vector<double> result;
    for (const auto &ev : trace.events()) {
        if (ev.kind == kind && !std::isnan(ev.t)) {
            result.push_back(ev.t);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

Check s1_bar_mapping(const TraceReader &trace, const Blocks &blocks) {
    const Table &S = trace.sensor();
    const Table &T = trace.transport();

    const auto &s_play_state = get_col(S, "play_state");
    const auto &s_r_measure = get_col(S, "r_measure");
    const auto &s_qn = get_col(S, "qn");
    const auto &s_ts_num = get_col(S, "ts_num");
    const auto &s_ts_den = get_col(S, "ts_den");
    const auto &s_r_qn = get_col(S, "r_qn");
    const auto &s_t = get_col(S, "t");
    const auto &s_play_pos = get_col(S, "play_pos");

    const auto &t_play_state = get_col(T, "play_state");
    const auto &t_t = get_col(T, "t");
    const auto &t_rate = get_col(T, "rate");
    const auto &t_pos2 = get_col(T, "pos2");

    int agree = 0;
    int total = 0;
    std::vector<std::pair<int, int>> mismatches;
    std::vector<double> qn_diff;

    // Compare the sensor's bar with the host's bar for every playing block.
    for (std::size_t i = 0; i < S.size(); ++i) {
        if (!is_playing(s_play_state[i]) || std::isnan(s_r_measure[i])) {
            continue;
        }
        ++total;
        int rb = static_cast<int>(s_r_measure[i]) + 1;

        double q = s_qn[i];
        int nb = naive_bar(q, s_ts_num[i], s_ts_den[i]);

        // A block that starts on a bar line may round to either side; both readings agree.
        int nb_minus = naive_bar(q - BAR_LINE_QN, s_ts_num[i], s_ts_den[i]);
        int nb_plus = naive_bar(q + BAR_LINE_QN, s_ts_num[i], s_ts_den[i]);
        std::vector<int> near = {nb_minus, nb_plus};

        bool matches = false;
        for (int n : near) {
            if (n == rb) {
                matches = true;
                break;
            }
        }
        if (nb == rb) matches = true;

        if (matches) {
            ++agree;
        } else if (mismatches.empty() || mismatches.back().second != rb) {
            mismatches.emplace_back(nb, rb);  // one entry per stretch of disagreement
        }

        if (!std::isnan(s_r_qn[i])) {
            qn_diff.push_back(std::abs(s_qn[i] - s_r_qn[i]));
        }
    }

    if (total == 0) {
        return Check("S1 bar mapping", "no data",
                     {"No playing blocks with host positions in this trace."});
    }

    // Sensor position versus the host's play position, at the transport sample times.
    std::vector<double> ts(s_t.begin(), s_t.end());
    std::vector<double> offsets;

    for (std::size_t k = 0; k < T.size(); ++k) {
        if (!is_playing(t_play_state[k])) {
            continue;
        }
        // Binary search: sensor times are in order
        auto it = std::upper_bound(ts.begin(), ts.end(), t_t[k]);
        if (it == ts.begin()) continue;
        std::size_t j = static_cast<std::size_t>(it - ts.begin()) - 1;

        if (j >= S.size() || !is_playing(s_play_state[j])) {
            continue;
        }
        int pid = blocks.pass_id()[j];
        if (pid == 0 || t_t[k] - ts[j] > 0.1) {
            continue;
        }
        double rate = std::isnan(t_rate[k]) ? 1.0 : t_rate[k];
        double sensor_pos = s_play_pos[j] + (t_t[k] - ts[j]) * rate;
        offsets.push_back(sensor_pos - t_pos2[k]);
    }

    double share = static_cast<double>(agree) / total;
    // Never round a partial agreement up to 100%.
    std::string pct = (agree == total) ? "100%" : fixed(std::min(share, 0.9999) * 100.0, 2) + "%";

    std::vector<std::string> lines;
    lines.push_back("Bar from the sensor's quarter notes agrees with the host's bar in " + pct +
                    " of " + with_commas(total) + " playing blocks.");

    if (!mismatches.empty()) {
        std::string shown;
        for (size_t i = 0; i < mismatches.size() && i < 4; ++i) {
            if (i > 0) shown += ", ";
            shown += "sensor " + std::to_string(mismatches[i].first) + " vs host " + std::to_string(mismatches[i].second);
        }
        if (mismatches.size() > 4) shown += " ...";
        lines.push_back("Disagreements start at: " + shown + ".");
        lines.push_back("A plugin only knows quarter notes and the current signature; after a signature change or with a measure"
                        " offset it needs the host's bar start (VST3 barPositionMusic, CLAP bar_start).");
    }

    if (!qn_diff.empty()) {
        double max_qn = *std::max_element(qn_diff.begin(), qn_diff.end());
        lines.push_back("Sensor quarter notes vs the host's for the same time: max difference " +
                        fixed(max_qn, 6) + " QN.");
    }

    if (!offsets.empty()) {
        double med = median(offsets);
        std::vector<double> abs_offsets;
        abs_offsets.reserve(offsets.size());
        for (double o : offsets) abs_offsets.push_back(std::abs(o));
        double p99 = quantile(abs_offsets, 0.99);
        lines.push_back("Sensor position minus the host's play position: median " +
                        signed_fixed(med * 1000.0, 1) + " ms,"
                        " p99 |offset| " + fixed(p99 * 1000.0, 1) + " ms (" +
                        std::to_string(offsets.size()) + " samples).");
        if (med > 0.05) {
            lines.push_back("The sensor runs well ahead of the next processed block: the host is rendering this chain ahead"
                            " (such as REAPER's anticipative FX), so its callbacks are not paced by the audio device.");
        }
    }

    std::string status = (share >= 0.999) ? "pass" : "fail";

    // Build JSON data
    std::vector<std::pair<std::string, JsonValue>> data_pairs;
    data_pairs.emplace_back("agreement", JsonValue(share));
    data_pairs.emplace_back("blocks", JsonValue(static_cast<double>(total)));

    std::vector<JsonValue> mismatches_json;
    for (size_t i = 0; i < mismatches.size() && i < 20; ++i) {   // each one is [sensor's bar, host's bar]
        mismatches_json.emplace_back(std::vector<JsonValue>{JsonValue(static_cast<double>(mismatches[i].first)),
                                                            JsonValue(static_cast<double>(mismatches[i].second))});
    }
    data_pairs.emplace_back("mismatches", JsonValue(mismatches_json));

    if (!qn_diff.empty()) {
        data_pairs.emplace_back("max_qn_diff", JsonValue(*std::max_element(qn_diff.begin(), qn_diff.end())));
    } else {
        data_pairs.emplace_back("max_qn_diff", JsonValue());
    }

    if (!offsets.empty()) {
        data_pairs.emplace_back("median_offset_s", JsonValue(median(offsets)));
    } else {
        data_pairs.emplace_back("median_offset_s", JsonValue());
    }

    return Check("S1 bar mapping", status, lines, JsonValue(data_pairs));
}

Check s2_vs_realtime(const TraceReader &trace,
                     const std::map<int, Cell> &late_heat,
                     const std::map<int, Cell> &rt_heat) {
    // Bars with both measurements
    std::vector<int> bars;
    for (const auto &kv : late_heat) {
        int b = kv.first;
        auto it = rt_heat.find(b);
        if (it != rt_heat.end() && kv.second.value.has_value() && it->second.value.has_value()) {
            bars.push_back(b);
        }
    }

    if (bars.size() < 3) {
        return Check("S2 proxy vs realtime CPU", "no data",
                     {"Fewer than three bars with both measurements."});
    }

    std::vector<double> xs, ys;
    xs.reserve(bars.size());
    ys.reserve(bars.size());
    for (int b : bars) {
        xs.push_back(rt_heat.at(b).value.value());
        ys.push_back(late_heat.at(b).value.value());
    }

    double rho = spearman(xs, ys);

    std::vector<std::string> lines;
    lines.push_back("Rank correlation, per bar, between the host's realtime CPU p99 and the lateness proxy p99: " +
                    signed_fixed(rho, 2) + " (" + std::to_string(bars.size()) + " bars).");

    // Does the proxy separate bars at or above 85% realtime CPU?
    std::vector<int> hot;
    for (int b : bars) {
        if (rt_heat.at(b).value.value() >= 0.85) {
            hot.push_back(b);
        }
    }
    if (!hot.empty()) {
        std::vector<double> hot_vals, cold_vals;
        for (int b : hot) hot_vals.push_back(late_heat.at(b).value.value());
        for (int b : bars) {
            if (rt_heat.at(b).value.value() < 0.85) {
                cold_vals.push_back(late_heat.at(b).value.value());
            }
        }
        double hot_med = median(hot_vals);
        double cold_med = cold_vals.empty() ? std::numeric_limits<double>::quiet_NaN() : median(cold_vals);
        lines.push_back("Bars at or above 85% realtime CPU: " + std::to_string(hot.size()) +
                        "; the proxy's median there is " + fixed(hot_med * 100.0, 0) + "% of a block, elsewhere " +
                        fixed(cold_med * 100.0, 0) + "%.");
    }

    std::string status;
    if (dummy_device(trace)) {
        status = "inconclusive";
        lines.push_back(DUMMY_NOTE);
    } else {
        status = rho >= 0.8 ? "pass" : rho >= 0.4 ? "inconclusive" : "fail";
    }

    std::vector<std::pair<std::string, JsonValue>> data_pairs;
    data_pairs.emplace_back("spearman", JsonValue(rho));
    data_pairs.emplace_back("bars", JsonValue(static_cast<double>(bars.size())));
    return Check("S2 proxy vs realtime CPU", status, lines, JsonValue(data_pairs));
}

Check s2_proxy(const TraceReader &trace, const Blocks &blocks, int min_blocks) {
    const Metric &late = metric_by_key("lateness");
    const Metric &workload = metric_by_key("workload");

    // Check if all workload values are NaN
    bool all_nan = true;
    for (double w : blocks.workload_share()) {
        if (!std::isnan(w)) {
            all_nan = false;
            break;
        }
    }
    if (all_nan) {
        return Check("S2 lateness proxy", "no data",
                     {"No known workload in this trace, so there is nothing to compare with. The REAPER fixtures F7a and F7b have one."});
    }

    // Per-pass, per-bar cells for lateness (p99) and workload (median)
    PassCells lat_cells = bar_cells(blocks.pass_id(), blocks.bar(), metric_values(blocks, late), 0.99, min_blocks);
    PassCells wl_cells = bar_cells(blocks.pass_id(), blocks.bar(), metric_values(blocks, workload), 0.5, min_blocks);

    std::vector<double> xs, ys;
    std::map<double, std::vector<double>> by_step;  // rounded workload step -> lateness values

    for (const auto &pid_pair : lat_cells) {
        int pid = pid_pair.first;
        const auto &lat_bars = pid_pair.second;
        auto wl_it = wl_cells.find(pid);
        if (wl_it == wl_cells.end()) continue;
        const auto &wl_bars = wl_it->second;

        for (const auto &bar_pair : lat_bars) {
            int b = bar_pair.first;
            const Cell &c = bar_pair.second;
            auto w_it = wl_bars.find(b);
            if (w_it == wl_bars.end()) continue;
            const Cell &w = w_it->second;
            if (!c.value.has_value() || !w.value.has_value()) continue;

            double w_val = w.value.value();
            double c_val = c.value.value();
            xs.push_back(w_val);
            ys.push_back(c_val);
            by_step[rounded_to_decimals(w_val, 2)].push_back(c_val);   // load steps, rounded to 1%
        }
    }

    if (xs.size() < 3) {
        return Check("S2 lateness proxy", "no data",
                     {"Fewer than three bars with enough blocks; record longer."});
    }

    double rho = spearman(xs, ys);

    // Which load step was playing when each of the host's xruns happened.
    std::vector<double> xr = xrun_times(trace);
    const auto &s_t = get_col(trace.sensor(), "t");
    std::vector<double> ts(s_t.begin(), s_t.end());
    std::map<double, int> xrun_steps;  // rounded workload step -> count

    for (double x : xr) {
        auto it = std::upper_bound(ts.begin(), ts.end(), x);
        if (it == ts.begin()) continue;
        std::size_t j = static_cast<std::size_t>(it - ts.begin()) - 1;
        if (j < blocks.workload_share().size() && !std::isnan(blocks.workload_share()[j])) {
            xrun_steps[rounded_to_decimals(blocks.workload_share()[j], 2)] += 1;
        }
    }

    std::vector<std::string> lines;
    // Header line
    lines.push_back(padded_left("Known load", 11) + "  " +
                    padded_left("bars", 4) + "  " +
                    padded_left("late p99 (median of bars)", 26) + "  " +
                    padded_left("Host xruns", 12));

    for (const auto &step_pair : by_step) {
        double step = step_pair.first;
        const auto &v = step_pair.second;
        double med = median(v);
        int xruns = 0;
        auto xr_it = xrun_steps.find(step);
        if (xr_it != xrun_steps.end()) xruns = xr_it->second;

        lines.push_back(padded_left(fixed(step * 100.0, 0) + "%", 10) + "  " +
                        padded_left(std::to_string(v.size()), 4) + "  " +
                        padded_left(fixed(med * 100.0, 1) + "%", 25) + "  " +
                        padded_left(std::to_string(xruns), 12));
    }

    lines.push_back("Rank correlation between known load and the lateness proxy, per bar: " +
                    signed_fixed(rho, 2) + " (" + std::to_string(xs.size()) + " bars).");

    // Where the proxy first leaves its idle level
    std::vector<double> steps;
    steps.reserve(by_step.size());
    for (const auto &kv : by_step) steps.push_back(kv.first);
    if (!steps.empty()) {
        double base = median(by_step[steps[0]]);
        std::optional<double> rises;
        for (size_t i = 1; i < steps.size(); ++i) {
            double med = median(by_step[steps[i]]);
            if (med > std::max(2.0 * base, base + 0.05)) {
                rises = steps[i];
                break;
            }
        }
        if (rises.has_value()) {
            lines.push_back("The proxy stays near its idle level (" + fixed(base * 100.0, 1) + "%) until " +
                            fixed(rises.value() * 100.0, 0) + "% known load, then rises: " +
                            "it signals deadline pressure, not load in general.");
        }
    }

    if (!xr.empty()) {
        lines.push_back("The host reported " + std::to_string(xr.size()) + " audio xrun(s) while recording.");
    }

    std::string status;
    if (dummy_device(trace)) {
        status = "inconclusive";
        lines.push_back(DUMMY_NOTE + " Repeat on a real interface.");
    } else if (std::isnan(rho)) {
        status = "inconclusive";
        lines.push_back("The proxy did not vary at all: it carries no information about load here.");
    } else if (rho >= 0.8) {
        status = "pass";
        lines.push_back("Reading: the proxy rises with the known load in this configuration.");
    } else if (rho >= 0.4) {
        status = "inconclusive";
        lines.push_back("Reading: a weak relation. Repeat with anticipative FX off (F7b) and a smaller buffer before deciding.");
    } else {
        status = "fail";
        lines.push_back("Reading: the proxy does not follow the load here. If anticipative FX is on, that is expected (blueprint R11).");
    }

    // Build JSON data
    std::vector<std::pair<std::string, JsonValue>> data_pairs;
    data_pairs.emplace_back("spearman", JsonValue(rho));
    data_pairs.emplace_back("bars", JsonValue(static_cast<double>(xs.size())));
    data_pairs.emplace_back("xruns", JsonValue(static_cast<double>(xr.size())));

    std::vector<std::pair<std::string, JsonValue>> steps_obj;
    for (const auto &kv : by_step) {
        steps_obj.emplace_back(float_text(kv.first), JsonValue(median(kv.second)));
    }
    data_pairs.emplace_back("steps", JsonValue(steps_obj));

    return Check("S2 lateness proxy", status, lines, JsonValue(data_pairs));
}

}  // namespace sessionscope
