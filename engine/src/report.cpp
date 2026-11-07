// The trace report implementation. report.h describes what it does.

#include "report.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "text_format.h"

namespace sessionscope {

namespace {

constexpr const char *MIDDLE_DOT = "\xC2\xB7";
constexpr const char *EN_DASH = "\xE2\x80\x93";
constexpr const char *MICRO = "\xC2\xB5";

template <typename Source>
Lane build_lane(const Source &blocks, const Metric &metric, double q, int min_blocks) {
    PassCells per_pass = bar_cells(blocks.pass_id(), blocks.bar(), metric_values(blocks, metric), q, min_blocks);
    BarCells heat = across_passes(per_pass);
    const std::optional<std::pair<int, int>> hot = hottest(heat, metric);
    return Lane{&metric, std::move(per_pass), std::move(heat), hot};
}

std::string joined_lines(const std::vector<std::string> &lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) text += '\n';
        text += lines[i];
    }
    return text;
}

// A trace named "folder/" still has a name.
std::string trace_name(const TraceReader &trace) {
    const std::filesystem::path &folder = trace.folder();
    return (folder.has_filename() ? folder.filename() : folder.parent_path().filename()).u8string();
}

// A value from meta.json as text: what a missing key, a null and a number each look like here.
std::string meta_text(const JsonValue &parent, std::string_view key, const std::string &if_missing) {
    const std::vector<std::string> keys = parent.keys();
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) return if_missing;
    const JsonValue &value = parent[key];
    if (value.is_null()) return "None";
    return value.is_number() ? json_number_text(value.number()) : value.text();
}

// 0, missing and null all count as "no value" for the sample rate, block size and duration.
bool has_nonzero_number(const JsonValue &value) { return value.is_number() && value.number() != 0; }

std::string percentile_name(double q) {
    return "p" + std::to_string(static_cast<long long>(py_round(q * 100.0)));
}

}  // namespace

Lane lane(const Blocks &blocks, const Metric &metric, double q, int min_blocks) {
    return build_lane(blocks, metric, q, min_blocks);
}

Lane lane(const HostBlocks &blocks, const Metric &metric, double q, int min_blocks) {
    return build_lane(blocks, metric, q, min_blocks);
}

Summary summarize(const TraceReader &trace, const Blocks &blocks,
                  std::string_view metric_key, int min_blocks, double q) {
    HostBlocks host_blocks(trace.host(), trace.transport());
    bool has_host = trace.host().size() > 0;
    if (has_host) {
        host_blocks.update();
    }

    if (metric_key == "host_rt" && !has_host) {
        throw std::runtime_error("This trace has no host.csv: only the optional REAPER extension writes it.");
    }

    const Metric &main_metric = metric_by_key(metric_key);
    Lane main = has_host && metric_key == "host_rt"
        ? lane(host_blocks, main_metric, q, min_blocks)
        : lane(blocks, main_metric, q, min_blocks);

    std::optional<Lane> rt;
    if (has_host && metric_key != "host_rt") {
        const Metric &rt_metric = metric_by_key("host_rt");
        rt = lane(host_blocks, rt_metric, q, min_blocks);
    }

    Lane late = (metric_key == "lateness") ? main : lane(blocks, metric_by_key("lateness"), q, min_blocks);

    const BarCells *rt_heat = nullptr;
    if (has_host) {
        if (metric_key == "host_rt") {
            rt_heat = &main.heat;
        } else if (rt.has_value()) {
            rt_heat = &rt->heat;
        }
    }

    std::vector<Check> checks;
    checks.push_back(s1_bar_mapping(trace, blocks));
    checks.push_back(s2_proxy(trace, blocks, min_blocks));
    if (rt_heat) {
        checks.push_back(s2_vs_realtime(trace, late.heat, *rt_heat));
    }

    std::vector<double> self_us;
    const auto &self_us_col = trace.sensor()["self_us"];
    self_us.reserve(self_us_col.size());
    for (double v : self_us_col) {
        if (!std::isnan(v)) self_us.push_back(v);
    }

    std::vector<double> xruns = xrun_times(trace);

    return Summary{std::move(main), std::move(rt), has_host ? std::make_optional(std::move(host_blocks)) : std::nullopt,
                   std::move(checks), std::move(self_us), std::move(xruns)};
}

std::optional<std::pair<int, int>> bar_of_time(const TraceReader &trace, const Blocks &blocks, double t) {
    const auto &ts = trace.sensor()["t"];
    auto it = std::upper_bound(ts.begin(), ts.end(), t);
    if (it == ts.begin()) {
        return std::nullopt;
    }
    std::size_t j = static_cast<std::size_t>(it - ts.begin()) - 1;
    if (j >= blocks.bar().size()) {
        return std::nullopt;
    }
    return std::make_pair(blocks.bar()[j], blocks.pass_id()[j]);
}

std::vector<std::string> heat_section(const Style &style, const Lane &lane, double q, int max_pass_rows) {
    const Metric &metric = *lane.metric;
    std::vector<std::string> out;
    out.push_back(style.paint(metric.title + ": " + percentile_name(q) + " per bar, " + metric.unit, nullptr, false, true));

    std::vector<std::pair<std::string, BarCells>> rows;
    for (const auto &kv : lane.per_pass) {
        rows.emplace_back("Pass " + std::to_string(kv.first), kv.second);
    }
    // Keep only the last max_pass_rows
    if (static_cast<int>(rows.size()) > max_pass_rows) {
        rows.erase(rows.begin(), rows.end() - max_pass_rows);
    }

    if (lane.per_pass.size() > 1) {
        rows.emplace_back("Median", lane.heat);
    }

    std::vector<std::string> hm = heatmap(style, metric, rows, 1, std::nullopt, 10);
    out.insert(out.end(), hm.begin(), hm.end());
    out.push_back(std::string(10, ' ') + legend(style, metric));
    return out;
}

std::string report_text(const TraceReader &trace,
                        std::string_view metric_key,
                        int min_blocks,
                        double q,
                        const Style *style,
                        int max_pass_rows) {
    Style local_style = style ? *style : Style();
    Blocks blocks = build_blocks(trace.sensor());
    const JsonValue m = parse_json(trace.meta_json()).value_or(JsonValue());
    const JsonValue &dev = m["device"];
    const JsonValue &host = m["host"];

    std::vector<std::string> out;

    std::string head = "SessionScope trace " + trace_name(trace);
    if (m["synthetic"].boolean()) {
        head += "  [SYNTHETIC: generated, not measured]";
    }
    out.push_back(local_style.paint(head, nullptr, false, true));

    const JsonValue &srate = dev["srate"];
    const JsonValue &bsize = dev["bsize"];
    const bool has_rate_and_size = has_nonzero_number(srate) && has_nonzero_number(bsize);
    const std::string srate_text = has_nonzero_number(srate) ? json_number_text(srate.number()) : "?";
    const std::string bsize_text = has_nonzero_number(bsize) ? json_number_text(bsize.number()) : "?";
    const std::string deadline =
        has_rate_and_size ? " (" + fixed(1000.0 * bsize.number() / srate.number(), 2) + " ms deadline)" : "";
    const std::string &project_name = m["project"]["name"].text();
    out.push_back(meta_text(host, "name", "?") + " " + meta_text(host, "version", "") + " " + MIDDLE_DOT + " " +
                  meta_text(dev, "mode", "?") + " " + srate_text + " Hz, " + bsize_text + " samples" + deadline + " " +
                  MIDDLE_DOT + " project " + (project_name.empty() ? "(unsaved)" : project_name));

    if (blocks.size() == 0) {
        out.push_back("");
        out.push_back("No sensor data. Is SessionScope Sensor on the master track, and was audio running?");
        return joined_lines(out);
    }

    Summary s = summarize(trace, blocks, metric_key, min_blocks, q);
    const JsonValue &dur = m["duration_s"];
    const std::string separator = std::string(" ") + MIDDLE_DOT + " ";
    out.push_back(with_commas(static_cast<long long>(blocks.size())) + " sensor blocks" +
                  (has_nonzero_number(dur) ? " over " + fixed(dur.number(), 0) + " s" : "") + separator +
                  std::to_string(blocks.missing()) + " lost" + separator +
                  std::to_string(blocks.resets()) + " sensor restarts" + separator +
                  std::to_string(blocks.passes().size()) + " passes" + separator +
                  // A plug-in cannot see the audio driver's xruns, so a count would mislead.
                  (dev["mode"].text() == "sensor" ? std::string("xruns not measured")
                                                  : std::to_string(s.xruns.size()) + " host audio xruns") +
                  (trace.host().size() > 0
                       ? separator + with_commas(static_cast<long long>(trace.host().size())) + " host blocks timed"
                       : ""));

    if (dummy_device(trace)) {
        out.push_back(local_style.paint("Note: " + DUMMY_NOTE, nullptr, true));
    }

    out.push_back("");
    out.push_back(local_style.paint("Passes", nullptr, false, true));
    out.push_back("   #  how it began  bars       blocks");
    for (const auto &p : blocks.passes()) {
        out.push_back(padded_left(std::to_string(p.id), 4) + "  " +
                      padded_right(p.kind, 12) + "  " +
                      padded_left(std::to_string(p.first_bar), 3) + EN_DASH +
                      padded_right(std::to_string(p.last_bar), 5) + "  " +
                      padded_left(with_commas(static_cast<long long>(p.blocks())), 7));
    }

    out.push_back("");
    std::vector<std::string> main_heat = heat_section(local_style, s.main, q, max_pass_rows);
    out.insert(out.end(), main_heat.begin(), main_heat.end());
    if (s.rt.has_value()) {
        out.push_back("");
        std::vector<std::string> rt_heat = heat_section(local_style, s.rt.value(), q, max_pass_rows);
        out.insert(out.end(), rt_heat.begin(), rt_heat.end());
    }

    out.push_back("");
    out.push_back(local_style.paint("Observations", nullptr, false, true));
    std::vector<std::string> obs = findings(trace, blocks, s, q);
    for (const auto &line : obs) {
        out.push_back("  - " + line);
    }

    for (const auto &c : s.checks) {
        out.push_back("");
        std::string status_upper = c.status;
        std::transform(status_upper.begin(), status_upper.end(), status_upper.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        out.push_back(local_style.paint(c.name + ": " + status_upper, nullptr, false, true));
        for (const auto &line : c.lines) {
            out.push_back("  " + line);
        }
    }

    return joined_lines(out);
}

std::optional<std::string> hot_line(const Lane &lane, double q) {
    const Metric &metric = *lane.metric;
    const BarCells &heat = lane.heat;

    int passes = 0;
    for (const auto &kv : heat) {
        if (kv.second.passes > passes) {
            passes = kv.second.passes;
        }
    }

    if (lane.hot.has_value()) {
        int a = lane.hot->first;
        int b = lane.hot->second;
        double peak = -std::numeric_limits<double>::infinity();
        for (int x = a; x <= b; ++x) {
            auto it = heat.find(x);
            if (it != heat.end() && it->second.value.has_value()) {
                peak = std::max(peak, it->second.value.value());
            }
        }
        std::string where = (a != b) ? "Bars " + std::to_string(a) + EN_DASH + std::to_string(b) + " are"
                                     : "Bar " + std::to_string(a) + " is";
        std::string lower_title = metric.title;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return where + " the hottest stretch: " + lower_title + " " + percentile_name(q) +
               " up to " + fmt(metric, peak) + " (" + TIER_NAMES[tier(metric, peak)] + "), median of up to " +
               std::to_string(passes) + " pass(es).";
    }

    double max_val = -std::numeric_limits<double>::infinity();
    bool has_val = false;
    for (const auto &kv : heat) {
        if (kv.second.value.has_value()) {
            max_val = std::max(max_val, kv.second.value.value());
            has_val = true;
        }
    }
    if (has_val) {
        return metric.title + ": no stretch reaches 'at risk'; the highest bar is " + fmt(metric, max_val) + ".";
    }
    return std::nullopt;
}

std::vector<std::string> findings(const TraceReader &trace, const Blocks &blocks, const Summary &s, double q) {
    std::vector<std::string> out;
    if (auto line = hot_line(s.main, q)) out.push_back(*line);
    if (s.rt.has_value()) {
        if (auto line = hot_line(s.rt.value(), q)) out.push_back(*line);
    }

    int few = 0;
    for (const auto &kv : s.main.heat) {
        if (!kv.second.value.has_value()) ++few;
    }
    if (few) {
        out.push_back(std::to_string(few) + " bar(s) had too few blocks for a " + percentile_name(q) +
                      "; they show as '" + MIDDLE_DOT + "', not as zero.");
    }

    for (size_t i = 0; i < s.xruns.size() && i < 5; ++i) {
        const auto bar_pass = bar_of_time(trace, blocks, s.xruns[i]);
        if (bar_pass.has_value() && bar_pass->first != 0) {   // bar 0 means "not in a known bar"
            out.push_back("The host reported an audio xrun at bar " + std::to_string(bar_pass->first) +
                          " (pass " + std::to_string(bar_pass->second) + ").");
        } else {
            out.push_back("The host reported an audio xrun.");
        }
    }
    if (s.xruns.size() > 5) {
        out.push_back("... and " + std::to_string(s.xruns.size() - 5) + " more xruns.");
    }

    if (!s.self_us.empty()) {
        double med = median(s.self_us);
        double p99 = quantile(s.self_us, 0.99);
        double mx = *std::max_element(s.self_us.begin(), s.self_us.end());
        out.push_back("Sensor's own cost per block: median " + fixed(med, 1) + " " + MICRO + "s, p99 " +
                      fixed(p99, 1) + " " + MICRO + "s, max " + fixed(mx, 1) + " " + MICRO + "s.");
    }

    if (blocks.missing()) {
        out.push_back(std::to_string(blocks.missing()) +
                      " sensor records were lost; no lateness is computed across those gaps.");
    }

    // The extension reports a running total. A blank cell (NaN) compares false, so it is skipped
    // and never becomes a number.
    double lost_host = 0;
    if (trace.host().size() > 0) {
        for (const double lost : trace.host()["lost"]) {
            if (lost > lost_host) lost_host = lost;
        }
    }
    if (lost_host > 0) {
        out.push_back("The extension's ring overflowed: " + std::to_string(static_cast<long long>(lost_host)) +
                      " host blocks were not timed.");
    }
    return out;
}

namespace {

// Convert a Cell to JSON object
JsonValue cell_to_json(const Cell &cell) {
    std::vector<std::pair<std::string, JsonValue>> obj;
    if (cell.value.has_value()) {
        obj.emplace_back("value", JsonValue(cell.value.value()));
    } else {
        obj.emplace_back("value", JsonValue());
    }
    obj.emplace_back("n", JsonValue(static_cast<double>(cell.n)));
    obj.emplace_back("passes", JsonValue(static_cast<double>(cell.passes)));
    return JsonValue(obj);
}

// Convert a BarCells map to JSON object with string keys
JsonValue bar_cells_to_json(const BarCells &cells) {
    std::vector<std::pair<std::string, JsonValue>> obj;
    for (const auto &kv : cells) {
        obj.emplace_back(std::to_string(kv.first), cell_to_json(kv.second));
    }
    return JsonValue(obj);
}

// Convert a PassCells map to JSON object with string keys
JsonValue pass_cells_to_json(const PassCells &cells) {
    std::vector<std::pair<std::string, JsonValue>> obj;
    for (const auto &kv : cells) {
        obj.emplace_back(std::to_string(kv.first), bar_cells_to_json(kv.second));
    }
    return JsonValue(obj);
}

// Convert a Lane to JSON object
JsonValue lane_to_json(const Lane &lane) {
    std::vector<std::pair<std::string, JsonValue>> obj;
    obj.emplace_back("metric", JsonValue(std::vector<std::pair<std::string, JsonValue>>{
        {"key", JsonValue(lane.metric->key)},
        {"title", JsonValue(lane.metric->title)},
        {"unit", JsonValue(lane.metric->unit)},
    }));
    obj.emplace_back("per_pass", pass_cells_to_json(lane.per_pass));
    obj.emplace_back("heat", bar_cells_to_json(lane.heat));
    if (lane.hot.has_value()) {
        obj.emplace_back("hottest", JsonValue(std::vector<JsonValue>{
            JsonValue(static_cast<double>(lane.hot->first)),
            JsonValue(static_cast<double>(lane.hot->second)),
        }));
    } else {
        obj.emplace_back("hottest", JsonValue());
    }
    return JsonValue(obj);
}

}  // namespace

JsonValue report_json(const TraceReader &trace,
                      std::string_view metric_key,
                      int min_blocks,
                      double q) {
    Blocks blocks = build_blocks(trace.sensor());
    Summary s = summarize(trace, blocks, metric_key, min_blocks, q);

    const Metric &main_metric = metric_by_key(metric_key);
    const JsonValue m = parse_json(trace.meta_json()).value_or(JsonValue());

    std::vector<std::pair<std::string, JsonValue>> result;
    result.emplace_back("schema", JsonValue(std::string("sessionscope.report")));
    result.emplace_back("schema_version", JsonValue(1.0));
    result.emplace_back("trace", JsonValue(trace_name(trace)));
    result.emplace_back("synthetic", JsonValue(m["synthetic"].boolean()));
    result.emplace_back("dummy_device", JsonValue(dummy_device(trace)));
    result.emplace_back("metric", JsonValue(std::vector<std::pair<std::string, JsonValue>>{
        {"key", JsonValue(main_metric.key)},
        {"title", JsonValue(main_metric.title)},
        {"unit", JsonValue(main_metric.unit)},
        {"quantile", JsonValue(q)},
        {"min_blocks", JsonValue(static_cast<double>(min_blocks))},
    }));
    result.emplace_back("blocks", JsonValue(static_cast<double>(blocks.size())));
    result.emplace_back("lost", JsonValue(static_cast<double>(blocks.missing())));
    result.emplace_back("sensor_restarts", JsonValue(static_cast<double>(blocks.resets())));

    std::vector<JsonValue> passes_json;
    for (const auto &p : blocks.passes()) {
        passes_json.emplace_back(JsonValue(std::vector<std::pair<std::string, JsonValue>>{
            {"id", JsonValue(static_cast<double>(p.id))},
            {"kind", JsonValue(p.kind)},
            {"first_bar", JsonValue(static_cast<double>(p.first_bar))},
            {"last_bar", JsonValue(static_cast<double>(p.last_bar))},
            {"blocks", JsonValue(static_cast<double>(p.blocks()))},
        }));
    }
    result.emplace_back("passes", JsonValue(passes_json));

    result.emplace_back("per_pass", pass_cells_to_json(s.main.per_pass));
    result.emplace_back("heat", bar_cells_to_json(s.main.heat));
    if (s.main.hot.has_value()) {
        result.emplace_back("hottest", JsonValue(std::vector<JsonValue>{
            JsonValue(static_cast<double>(s.main.hot->first)),
            JsonValue(static_cast<double>(s.main.hot->second)),
        }));
    } else {
        result.emplace_back("hottest", JsonValue());
    }

    if (s.rt.has_value()) {
        result.emplace_back("realtime", lane_to_json(s.rt.value()));
    } else {
        result.emplace_back("realtime", JsonValue());
    }

    std::vector<JsonValue> xruns_json;
    for (double x : s.xruns) {
        xruns_json.emplace_back(JsonValue(x));
    }
    result.emplace_back("xruns", JsonValue(xruns_json));

    std::vector<JsonValue> checks_json;
    for (const auto &c : s.checks) {
        checks_json.emplace_back(JsonValue(std::vector<std::pair<std::string, JsonValue>>{
            {"name", JsonValue(c.name)},
            {"status", JsonValue(c.status)},
            {"lines", JsonValue(c.lines)},
            {"data", c.data},
        }));
    }
    result.emplace_back("checks", JsonValue(checks_json));

    return JsonValue(result);
}

}  // namespace sessionscope
