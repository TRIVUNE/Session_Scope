// The HUD's modules: see modules.h.

#include "modules.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace sessionscope {

namespace {

// ------------------------------------------------------------------------------------ helpers

ImVec2 at(float x, float y) { return ImVec2(x, y); }

// A module's frame: white inside, one dark pixel around.
void panel(ImDrawList *draw, const Box &b) {
    draw->AddRectFilled(at(b.x, b.y), at(b.x + b.w, b.y + b.h), colour::PANEL);
    draw->AddRect(at(b.x, b.y), at(b.x + b.w, b.y + b.h), colour::BORDER);
}

// The inside of a frame, `inset` window units in from the border.
Box inside(const Box &b, float inset) { return {b.x + inset, b.y + inset, b.w - 2 * inset, b.h - 2 * inset}; }

// "12%" for 0.12; one decimal under 10 %, so small loads still move; a dash when unknown.
std::string percent(std::optional<double> share) {
    if (!share) return "\xE2\x80\x93";
    char text[16];
    const double value = *share * 100.0;
    std::snprintf(text, sizeof text, value < 9.95 ? "%.1f%%" : "%.0f%%", value);
    return text;
}

// The heatmap's colours, the "jet" scale of spectrum analysers: dark blue when idle, through
// blue, cyan, green and yellow, to red when the core is full.
ImU32 shade(float load) {
    struct Stop {
        float at;
        float r, g, b;
    };
    static constexpr std::array<Stop, 7> stops = {{{0.00f, 0, 0, 110},
                                                   {0.15f, 0, 20, 255},
                                                   {0.35f, 0, 200, 255},
                                                   {0.50f, 20, 230, 90},
                                                   {0.65f, 250, 250, 0},
                                                   {0.82f, 255, 130, 0},
                                                   {1.00f, 225, 0, 0}}};
    load = std::clamp(load, 0.0f, 1.0f);
    std::size_t i = 1;
    while (i + 1 < stops.size() && load > stops[i].at) ++i;
    const Stop &lo = stops[i - 1], &hi = stops[i];
    const float t = (load - lo.at) / (hi.at - lo.at);
    const auto mix = [t](float a, float b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return IM_COL32(mix(lo.r, hi.r), mix(lo.g, hi.g), mix(lo.b, hi.b), 255);
}

// The DAW's CPU averaged over the last `seconds` of readings; none if no reading has it.
std::optional<double> daw_average(const CpuMonitor &cpu, double now, double seconds) {
    double sum = 0;
    int count = 0;
    for (const CpuSample &sample : cpu.samples()) {
        if (sample.t >= now - seconds && sample.daw) {
            sum += *sample.daw;
            ++count;
        }
    }
    if (!count) return std::nullopt;
    return sum / count;
}

// ------------------------------------------------------------------------------------ meter

// The DAW's CPU as a level: rises within a frame or two, falls over about half a second, and a
// black mark stays at the highest level for two seconds before it sinks.
class PeakMeter final : public Module {
public:
    const char *id() const override { return "meter"; }
    const char *title() const override { return "Peak meter"; }
    float fixed_length(bool, float, float scale) const override { return 18 * scale; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.cpu.samples();
        const double target = !samples.empty() && samples.back().daw ? *samples.back().daw : 0.0;
        const double dt = last_frame_ < 0 ? 0 : std::max(0.0, frame.now - last_frame_);
        last_frame_ = frame.now;

        const double settle = target > level_ ? 0.04 : 0.45;   // seconds: fast up, slower down
        level_ += (target - level_) * (1.0 - std::exp(-dt / settle));
        if (target >= peak_) {
            peak_ = target;
            peak_time_ = frame.now;
        } else if (frame.now - peak_time_ > HOLD_S) {
            peak_ = std::max(target, peak_ - dt * 0.5);   // sinks at half the scale per second
        }

        panel(draw, box);
        const Box in = inside(box, 2 * frame.scale);
        const float level = static_cast<float>(std::clamp(level_, 0.0, 1.0));
        const float peak = static_cast<float>(std::clamp(peak_, 0.0, 1.0));
        const float mark = 2 * frame.scale;
        if (box.h >= box.w) {   // tall: fills upward
            const float bottom = in.y + in.h;
            draw->AddRectFilled(at(in.x, bottom - in.h * level), at(in.x + in.w, bottom), colour::FILL);
            if (peak > 0) {
                const float y = std::max(in.y, bottom - in.h * peak - mark * 0.5f);
                draw->AddRectFilled(at(in.x, y), at(in.x + in.w, y + mark), colour::MARK);
            }
        } else {   // wide: fills rightward
            draw->AddRectFilled(at(in.x, in.y), at(in.x + in.w * level, in.y + in.h), colour::FILL);
            if (peak > 0) {
                const float x = std::min(in.x + in.w - mark, in.x + in.w * peak - mark * 0.5f);
                draw->AddRectFilled(at(x, in.y), at(x + mark, in.y + in.h), colour::MARK);
            }
        }
    }

private:
    static constexpr double HOLD_S = 2.0;
    double level_ = 0, peak_ = 0, peak_time_ = -1e9, last_frame_ = -1;
};

// ------------------------------------------------------------------------------------ cpu number

// The DAW's CPU share of the machine, averaged over the last second, in Inter Display Light.
class CpuNumber final : public Module {
public:
    explicit CpuNumber(const Fonts &fonts) : fonts_(fonts) {}
    const char *id() const override { return "cpu"; }
    const char *title() const override { return "CPU number"; }

    float fixed_length(bool vertical, float thickness, float scale) const override {
        const float size = text_size(thickness, scale);
        if (vertical) return std::round(size * 1.25f);
        return std::round(text_width(fonts_.number, size, "100%") + 6 * scale);
    }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const bool vertical = box.h < box.w * 0.6f;   // laid out in a column: a wide, short box
        const float size = text_size(vertical ? box.w : box.h, frame.scale);
        const std::optional<double> share = daw_average(frame.cpu, frame.now, 1.0);
        const std::string text = percent(share);
        const float y = box.y + (box.h - size) * 0.5f - size * 0.04f;   // optical centre
        draw->AddText(fonts_.number, size, at(box.x + 2 * frame.scale, y), share ? colour::TEXT : colour::TEXT_QUIET,
                      text.c_str());
    }

private:
    static float text_size(float thickness, float scale) { return std::min(thickness * 0.62f, 72 * scale); }
    const Fonts &fonts_;
};

// ------------------------------------------------------------------------------------ cores

// One row per core, newest reading at the right edge, sliding left every display frame. The
// readings arrive ten times a second; the picture is placed by their times, so it moves smoothly
// in between. Bar starts are thin lines (white over the colours) with the bar number in a
// ruler above.
class CoreHeatmap final : public Module {
public:
    CoreHeatmap(const Fonts &fonts, TextureStore &textures) : fonts_(fonts), textures_(textures) {}
    ~CoreHeatmap() override {
        if (texture_) textures_.destroy_texture(texture_);
    }
    const char *id() const override { return "cores"; }
    const char *title() const override { return "Core heatmap"; }
    float fixed_length(bool, float, float) const override { return 0; }
    float weight() const override { return 3; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        panel(draw, box);
        const Box in = inside(box, 1);
        const float ruler = std::round(fonts_.text_size + 6 * frame.scale);
        const Box map = {in.x, in.y + ruler, in.w, in.h - ruler};
        draw->AddLine(at(map.x, map.y - 0.5f), at(map.x + map.w, map.y - 0.5f), colour::FILL);
        draw->AddRectFilled(at(map.x, map.y), at(map.x + map.w, map.y + map.h), shade(0));   // no readings yet: idle

        const auto &samples = frame.cpu.samples();
        upload(samples);
        if (samples.size() < 2 || !texture_) return;

        // Seconds per reading, and where a time falls on screen. The newest reading's column
        // reaches the right edge just as the next one arrives, so nothing jumps.
        const double step = std::max(0.02, (samples.back().t - samples.front().t) / (samples.size() - 1));
        const double shown_until = frame.now - step;
        const double span = frame.cpu.span();
        const auto x_of = [&](double t) { return static_cast<float>(map.x + map.w - (shown_until - t) / span * map.w); };

        draw->PushClipRect(at(in.x, in.y), at(in.x + in.w, in.y + in.h), true);
        draw->AddImage(texture_, at(x_of(first_column_t_ - step), map.y), at(x_of(last_column_t_), map.y + map.h));

        // Bar lines: where the bar playing changes between two readings.
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const std::optional<int> bar = samples[i].bar;
            if (!bar || samples[i - 1].bar == bar) continue;
            const float x = std::round(x_of(samples[i].t - step)) + 0.5f;
            draw->AddLine(at(x, in.y), at(x, map.y), colour::MARK);                   // in the ruler
            draw->AddLine(at(x, map.y), at(x, map.y + map.h), IM_COL32(255, 255, 255, 200));   // over the colours
            const std::string label = std::to_string(*bar);
            draw->AddText(fonts_.text, fonts_.text_size, at(x + 3 * frame.scale, in.y + (ruler - fonts_.text_size) * 0.5f),
                          colour::TEXT, label.c_str());
        }
        draw->PopClipRect();
    }

private:
    // Rebuilds the picture when a new reading has come: one column per reading, one row per core.
    void upload(const std::deque<CpuSample> &samples) {
        if (samples.empty() || samples.back().t == uploaded_t_) return;
        uploaded_t_ = samples.back().t;
        const std::size_t cores = samples.back().cores.size();
        if (!cores) return;

        std::vector<const CpuSample *> columns;
        for (const CpuSample &sample : samples) {
            if (sample.cores.size() == cores) columns.push_back(&sample);
        }
        const int width = static_cast<int>(columns.size()), height = static_cast<int>(cores);
        pixels_.assign(static_cast<std::size_t>(width) * height, 0);
        for (int c = 0; c < width; ++c) {
            for (int row = 0; row < height; ++row) {
                pixels_[static_cast<std::size_t>(row) * width + c] = shade(columns[c]->cores[row]);
            }
        }
        texture_ = textures_.upload_texture(texture_, width, height, pixels_.data());
        first_column_t_ = columns.front()->t;
        last_column_t_ = columns.back()->t;
    }

    const Fonts &fonts_;
    TextureStore &textures_;
    ImTextureID texture_ = 0;
    std::vector<std::uint32_t> pixels_;
    double uploaded_t_ = -1;
    double first_column_t_ = 0, last_column_t_ = 0;
};

// ------------------------------------------------------------------------------------ bars

// Every bar of the song, regrouped from the bar map the engine already sends (one row per pass
// plus the median): regrouping, not new measurement.
struct BarSummary {
    int bar = 0;
    std::optional<double> median;   // late p99, median over passes
    std::optional<double> worst;    // the worst pass
    int blocks = 0;
};

std::vector<BarSummary> summarise_bars(const std::vector<BarRow> &rows) {
    const bool has_median_row = std::any_of(rows.begin(), rows.end(), [](const BarRow &r) { return r.pass == 0; });
    std::map<int, BarSummary> by_bar;
    for (const BarRow &row : rows) {
        for (const auto &[bar, cell] : row.cells) {
            BarSummary &summary = by_bar[bar];
            summary.bar = bar;
            if (row.pass == 0) {
                summary.median = cell.value;
                continue;
            }
            summary.blocks += cell.n;
            if (cell.value) summary.worst = summary.worst ? std::max(*summary.worst, *cell.value) : *cell.value;
            if (!has_median_row) summary.median = cell.value;   // a single pass: its value is the median
        }
    }
    std::vector<BarSummary> out;
    for (const auto &[bar, summary] : by_bar) out.push_back(summary);
    return out;
}

// The Task Manager view: one row per bar, how late its blocks ran. Click a column title to sort
// by it; the bar playing now is shaded, values past the "at risk" tier sit on amber, overruns on red.
class BarList final : public Module {
public:
    explicit BarList(const Fonts &fonts) : fonts_(fonts) {}
    const char *id() const override { return "bars"; }
    const char *title() const override { return "Bars list"; }
    float fixed_length(bool, float, float) const override { return 0; }
    float weight() const override { return 1.4f; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        panel(draw, box);
        const Snapshot &s = frame.snapshot;
        const float size = fonts_.text_size;
        const float row_h = std::round(size * 1.8f);
        const Box in = inside(box, 1);

        // Columns: Bar, Late p99, Worst, Blocks. The first is left-aligned, numbers right.
        struct Column {
            const char *key;
            const char *name;
            float share;
        };
        static constexpr std::array<Column, 4> columns = {
            {{"bar", "Bar", 0.22f}, {"late", "Late p99", 0.28f}, {"worst", "Worst", 0.25f}, {"blocks", "Blocks", 0.25f}}};
        const float pad = 6 * frame.scale;

        float x = in.x;
        for (const Column &column : columns) {
            const float w = in.w * column.share;
            const bool sorted = sort_ == column.key;
            const std::string name = std::string(column.name) + (sorted && sort_ != "bar" ? " \xE2\x96\xBE" : "");
            const float tx = column.key == std::string("bar") ? x + pad : x + w - pad - text_width(fonts_.text, size, name.c_str());
            draw->AddText(fonts_.text, size, at(tx, in.y + (row_h - size) * 0.5f), sorted ? colour::TEXT : colour::TEXT_QUIET,
                          name.c_str());
            ImGui::SetCursorScreenPos(at(x, in.y));
            if (ImGui::InvisibleButton((std::string("##sort_") + column.key).c_str(), ImVec2(std::max(1.0f, w), row_h))) {
                sort_ = column.key;
            }
            x += w;
        }
        draw->AddLine(at(in.x, in.y + row_h - 0.5f), at(in.x + in.w, in.y + row_h - 0.5f), colour::FILL);

        std::vector<BarSummary> bars = s.ready ? summarise_bars(s.bars) : std::vector<BarSummary>{};
        if (bars.empty()) {
            const char *empty = "No bars yet";
            draw->AddText(fonts_.text, size, at(in.x + (in.w - text_width(fonts_.text, size, empty)) * 0.5f, in.y + row_h + pad),
                          colour::TEXT_QUIET, empty);
            return;
        }
        std::sort(bars.begin(), bars.end(), order());

        // The rows scroll under the fixed header.
        const float rows_top = in.y + row_h;
        ImGui::SetCursorScreenPos(at(in.x, rows_top));
        ImGui::BeginChild("##bars_rows", ImVec2(in.w, std::max(1.0f, in.y + in.h - rows_top)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        ImDrawList *rows = ImGui::GetWindowDrawList();
        const float at_risk = s.metric ? static_cast<float>(s.metric->tiers[1]) : 1e9f;
        const float overrun = s.metric ? static_cast<float>(s.metric->tiers[2]) : 1e9f;
        for (const BarSummary &bar : bars) {
            const ImVec2 row = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(in.w, row_h));
            if (!ImGui::IsItemVisible()) continue;
            if (s.playing && s.bar && *s.bar == bar.bar) rows->AddRectFilled(row, at(row.x + in.w, row.y + row_h), colour::ROW_NOW);

            const std::array<std::string, 4> cells = {std::to_string(bar.bar), percent(bar.median), percent(bar.worst),
                                                      std::to_string(bar.blocks)};
            const std::array<std::optional<double>, 4> values = {std::nullopt, bar.median, bar.worst, std::nullopt};
            float cx = row.x;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                const float w = in.w * columns[i].share;
                if (values[i] && *values[i] >= at_risk) {
                    rows->AddRectFilled(at(cx + 1, row.y + 1), at(cx + w - 1, row.y + row_h - 1),
                                        *values[i] >= overrun ? colour::OVERRUN : colour::AT_RISK);
                }
                const float tx = i == 0 ? cx + pad : cx + w - pad - text_width(fonts_.text, size, cells[i].c_str());
                rows->AddText(fonts_.text, size, at(tx, row.y + (row_h - size) * 0.5f), colour::TEXT, cells[i].c_str());
                cx += w;
            }
        }
        ImGui::EndChild();
    }

private:
    std::function<bool(const BarSummary &, const BarSummary &)> order() const {
        const auto descending = [](std::optional<double> a, std::optional<double> b) { return a.value_or(-1) > b.value_or(-1); };
        if (sort_ == "late") return [=](const BarSummary &a, const BarSummary &b) { return descending(a.median, b.median); };
        if (sort_ == "worst") return [=](const BarSummary &a, const BarSummary &b) { return descending(a.worst, b.worst); };
        if (sort_ == "blocks") return [](const BarSummary &a, const BarSummary &b) { return a.blocks > b.blocks; };
        return [](const BarSummary &a, const BarSummary &b) { return a.bar < b.bar; };
    }

    const Fonts &fonts_;
    std::string sort_ = "bar";
};

}  // namespace

std::vector<std::unique_ptr<Module>> make_modules(const Fonts &fonts, TextureStore &textures) {
    std::vector<std::unique_ptr<Module>> modules;
    modules.push_back(std::make_unique<PeakMeter>());
    modules.push_back(std::make_unique<CpuNumber>(fonts));
    modules.push_back(std::make_unique<CoreHeatmap>(fonts, textures));
    modules.push_back(std::make_unique<BarList>(fonts));
    return modules;
}

}  // namespace sessionscope
