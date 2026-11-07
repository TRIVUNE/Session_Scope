// The HUD's modules: see modules.h.

#include "modules.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

constexpr const char *DASH = "\xE2\x80\x93";   // an unknown value

// "12%" for 0.12; one decimal under 10 %, so small loads still move.
std::string percent(std::optional<double> share) {
    if (!share) return DASH;
    char text[16];
    const double value = *share * 100.0;
    std::snprintf(text, sizeof text, value < 9.95 ? "%.1f%%" : "%.0f%%", value);
    return text;
}

// "2.4 GB", "512 MB".
std::string bytes(std::optional<double> b) {
    if (!b) return DASH;
    char text[24];
    if (*b >= 1e9) std::snprintf(text, sizeof text, "%.1f GB", *b / 1e9);
    else std::snprintf(text, sizeof text, "%.0f MB", *b / 1e6);
    return text;
}

// "3.20 GHz".
std::string ghz(std::optional<double> mhz) {
    if (!mhz) return DASH;
    char text[24];
    std::snprintf(text, sizeof text, "%.2f GHz", *mhz / 1000.0);
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
std::optional<double> daw_average(const SystemMonitor &system, double now, double seconds) {
    double sum = 0;
    int count = 0;
    for (const SystemSample &sample : system.samples()) {
        if (sample.t >= now - seconds && sample.daw) {
            sum += *sample.daw;
            ++count;
        }
    }
    if (!count) return std::nullopt;
    return sum / count;
}

// The busiest core in a reading: its number and load.
std::optional<std::pair<int, float>> busiest_core(const SystemSample &sample) {
    if (sample.cores.empty()) return std::nullopt;
    const auto top = std::max_element(sample.cores.begin(), sample.cores.end());
    return std::make_pair(static_cast<int>(top - sample.cores.begin()), *top);
}

// A level that rises within a frame or two, falls over about half a second, and a peak mark that
// stays at the highest level for two seconds before it sinks.
class LevelFollower {
public:
    void follow(double target, double now) {
        const double dt = last_ < 0 ? 0 : std::max(0.0, now - last_);
        last_ = now;
        const double settle = target > level_ ? 0.04 : 0.45;   // seconds: fast up, slower down
        level_ += (target - level_) * (1.0 - std::exp(-dt / settle));
        if (target >= peak_) {
            peak_ = target;
            peak_time_ = now;
        } else if (now - peak_time_ > 2.0) {
            peak_ = std::max(target, peak_ - dt * 0.5);   // sinks at half the scale per second
        }
    }
    float level() const { return static_cast<float>(std::clamp(level_, 0.0, 1.0)); }
    float peak() const { return static_cast<float>(std::clamp(peak_, 0.0, 1.0)); }

private:
    double level_ = 0, peak_ = 0, peak_time_ = -1e9, last_ = -1;
};

// A level bar with its peak mark: fills upward when the box is tall, rightward when wide.
void level_bar(ImDrawList *draw, const Box &box, const LevelFollower &level, float scale) {
    panel(draw, box);
    const Box in = inside(box, 2 * scale);
    const float mark = 2 * scale;
    if (box.h >= box.w) {
        const float bottom = in.y + in.h;
        draw->AddRectFilled(at(in.x, bottom - in.h * level.level()), at(in.x + in.w, bottom), colour::FILL);
        if (level.peak() > 0) {
            const float y = std::max(in.y, bottom - in.h * level.peak() - mark * 0.5f);
            draw->AddRectFilled(at(in.x, y), at(in.x + in.w, y + mark), colour::MARK);
        }
    } else {
        draw->AddRectFilled(at(in.x, in.y), at(in.x + in.w * level.level(), in.y + in.h), colour::FILL);
        if (level.peak() > 0) {
            const float x = std::min(in.x + in.w - mark, in.x + in.w * level.peak() - mark * 0.5f);
            draw->AddRectFilled(at(x, in.y), at(x + mark, in.y + in.h), colour::MARK);
        }
    }
}

// ------------------------------------------------------------------------------------ cards

// The look shared by the small fixed modules (busiest core, dropouts, memory, clock): a frame with
// a quiet title, one large value, then a few label / value lines.
struct CardLine {
    std::string label;
    std::string value;
    ImU32 colour = colour::TEXT;
};

class Card : public Module {
public:
    explicit Card(const Fonts &fonts) : fonts_(fonts) {}

    float fixed_length(bool vertical, float, float scale) const override {
        if (!vertical) return std::round(150 * scale);
        return std::round(2 * pad(scale) + fonts_.text_size * 1.4f + value_size(scale) * 1.15f +
                          lines() * fonts_.text_size * 1.5f);
    }

protected:
    virtual int lines() const = 0;
    float pad(float scale) const { return 8 * scale; }
    float value_size(float scale) const { return 26 * scale; }

    // Draws the frame, title, value and lines; returns the space left below them.
    Box draw_card(ImDrawList *draw, const Box &box, float scale, const char *title, const std::string &value,
                  ImU32 value_colour, const std::vector<CardLine> &card_lines) const {
        panel(draw, box);
        const float p = pad(scale), size = fonts_.text_size;
        const float value_px = std::min(value_size(scale), box.h * 0.36f);
        float y = box.y + p * 0.75f;
        draw->PushClipRect(at(box.x + 1, box.y + 1), at(box.x + box.w - 1, box.y + box.h - 1), true);
        draw->AddText(fonts_.text, size, at(box.x + p, y), colour::TEXT_QUIET, title);
        y += size * 1.4f;
        draw->AddText(fonts_.number, value_px, at(box.x + p - value_px * 0.04f, y), value_colour, value.c_str());
        y += value_px * 1.15f;
        for (const CardLine &line : card_lines) {
            if (y + size > box.y + box.h - p * 0.5f) break;
            draw->AddText(fonts_.text, size, at(box.x + p, y), colour::TEXT_QUIET, line.label.c_str());
            const float w = text_width(fonts_.text, size, line.value.c_str());
            draw->AddText(fonts_.text, size, at(box.x + box.w - p - w, y), line.colour, line.value.c_str());
            y += size * 1.5f;
        }
        draw->PopClipRect();
        return {box.x, y, box.w, box.y + box.h - y};
    }

    const Fonts &fonts_;
};

// ------------------------------------------------------------------------------------ meter

class PeakMeter final : public Module {
public:
    const char *id() const override { return "meter"; }
    const char *title() const override { return "Peak meter (DAW CPU)"; }
    float fixed_length(bool, float, float scale) const override { return 18 * scale; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        level_.follow(!samples.empty() && samples.back().daw ? *samples.back().daw : 0.0, frame.now);
        level_bar(draw, box, level_, frame.scale);
    }

private:
    LevelFollower level_;
};

// ------------------------------------------------------------------------------------ cpu number

// The DAW's CPU share of the machine, averaged over the last second, in Inter Display Light.
class CpuNumber final : public Module {
public:
    explicit CpuNumber(const Fonts &fonts) : fonts_(fonts) {}
    const char *id() const override { return "cpu"; }
    const char *title() const override { return "DAW CPU"; }

    float fixed_length(bool vertical, float thickness, float scale) const override {
        const float size = text_size(thickness, scale);
        if (vertical) return std::round(size * 1.25f);
        return std::round(text_width(fonts_.number, size, "100%") + 6 * scale);
    }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const bool vertical = box.h < box.w * 0.6f;   // laid out in a column: a wide, short box
        const float size = text_size(vertical ? box.w : box.h, frame.scale);
        const std::optional<double> share = daw_average(frame.system, frame.now, 1.0);
        const std::string text = percent(share);
        const float y = box.y + (box.h - size) * 0.5f - size * 0.04f;   // optical centre
        draw->AddText(fonts_.number, size, at(box.x + 2 * frame.scale, y), share ? colour::TEXT : colour::TEXT_QUIET,
                      text.c_str());
        draw->AddText(fonts_.text, fonts_.text_size, at(box.x + 3 * frame.scale, box.y), colour::TEXT_QUIET, "DAW CPU");
    }

private:
    static float text_size(float thickness, float scale) { return std::min(thickness * 0.62f, 72 * scale); }
    const Fonts &fonts_;
};

// ------------------------------------------------------------------------------------ cores

// One row per core, numbered on the left, newest reading at the right edge, sliding left every
// display frame. The readings arrive ten times a second; the picture is placed by their times, so
// it moves smoothly in between. Bar starts are thin lines (white over the colours) with the bar
// number in a ruler above. Hover for the exact load of a core at a moment.
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
        const float size = fonts_.text_size;
        const float ruler = std::round(size + 6 * frame.scale);
        const float gutter = std::round(text_width(fonts_.text, size, "00") + 8 * frame.scale);
        const Box map = {in.x + gutter, in.y + ruler, in.w - gutter, in.h - ruler};
        draw->AddLine(at(in.x, map.y - 0.5f), at(in.x + in.w, map.y - 0.5f), colour::FILL);
        draw->AddLine(at(map.x - 0.5f, in.y), at(map.x - 0.5f, in.y + in.h), colour::FILL);
        draw->AddRectFilled(at(map.x, map.y), at(map.x + map.w, map.y + map.h), shade(0));   // no readings yet: idle
        draw->AddText(fonts_.text, size, at(in.x + 4 * frame.scale, in.y + (ruler - size) * 0.5f), colour::TEXT_QUIET, "core");

        const auto &samples = frame.system.samples();
        upload(samples);
        if (cores_ == 0) return;

        // Core numbers, as many as fit: every row, or every second, fourth... when rows are thin.
        const float row_h = map.h / cores_;
        int every = 1;
        while (row_h * every < size * 1.05f) every *= 2;
        for (int core = 0; core < cores_; core += every) {
            const std::string label = std::to_string(core);
            const float y = map.y + row_h * (core + 0.5f) - size * 0.5f;
            draw->AddText(fonts_.text, size, at(map.x - 4 * frame.scale - text_width(fonts_.text, size, label.c_str()), y),
                          colour::TEXT, label.c_str());
        }
        if (samples.size() < 2 || !texture_) return;

        // Seconds per reading, and where a time falls on screen. The newest reading's column
        // reaches the right edge just as the next one arrives, so nothing jumps.
        const double step = std::max(0.02, (samples.back().t - samples.front().t) / (samples.size() - 1));
        const double shown_until = frame.now - step;
        const double span = frame.system.span();
        const auto x_of = [&](double t) { return static_cast<float>(map.x + map.w - (shown_until - t) / span * map.w); };

        draw->PushClipRect(at(map.x, in.y), at(in.x + in.w, in.y + in.h), true);
        draw->AddImage(texture_, at(x_of(column_t_.front() - step), map.y), at(x_of(column_t_.back()), map.y + map.h));

        // Bar lines: where the bar playing changes between two readings.
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const std::optional<int> bar = samples[i].bar;
            if (!bar || samples[i - 1].bar == bar) continue;
            const float x = std::round(x_of(samples[i].t - step)) + 0.5f;
            draw->AddLine(at(x, in.y), at(x, map.y), colour::MARK);
            draw->AddLine(at(x, map.y), at(x, map.y + map.h), IM_COL32(255, 255, 255, 200));
            const std::string label = std::to_string(*bar);
            draw->AddText(fonts_.text, size, at(x + 3 * frame.scale, in.y + (ruler - size) * 0.5f), colour::TEXT,
                          label.c_str());
        }
        draw->PopClipRect();

        // Hover: which core, how busy, when.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.x >= map.x && mouse.x < map.x + map.w && mouse.y >= map.y && mouse.y < map.y + map.h &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const int core = std::clamp(static_cast<int>((mouse.y - map.y) / row_h), 0, cores_ - 1);
            const double t = shown_until - (map.x + map.w - mouse.x) / map.w * span;
            // The reading whose column (the step before it) covers t.
            const auto later = std::lower_bound(column_t_.begin(), column_t_.end(), t);
            if (later != column_t_.end()) {
                const std::size_t column = static_cast<std::size_t>(later - column_t_.begin());
                const float load = loads_[static_cast<std::size_t>(core) * column_t_.size() + column];
                draw->AddRect(at(map.x, map.y + row_h * core), at(map.x + map.w, map.y + row_h * (core + 1)),
                              IM_COL32(255, 255, 255, 160));
                ImGui::BeginTooltip();
                ImGui::Text("Core %d   %s", core, percent(load).c_str());
                ImGui::TextDisabled("%.1f s ago", std::max(0.0, frame.now - column_t_[column]));
                ImGui::EndTooltip();
            }
        }
    }

private:
    // Rebuilds the picture when a new reading has come: one column per reading, one row per core.
    void upload(const std::deque<SystemSample> &samples) {
        if (samples.empty() || samples.back().t == uploaded_t_) return;
        uploaded_t_ = samples.back().t;
        const std::size_t cores = samples.back().cores.size();
        if (!cores) return;

        column_t_.clear();
        std::vector<const SystemSample *> columns;
        for (const SystemSample &sample : samples) {
            if (sample.cores.size() == cores) {
                columns.push_back(&sample);
                column_t_.push_back(sample.t);
            }
        }
        const int width = static_cast<int>(columns.size()), height = static_cast<int>(cores);
        pixels_.assign(static_cast<std::size_t>(width) * height, 0);
        loads_.assign(pixels_.size(), 0);
        for (int c = 0; c < width; ++c) {
            for (int row = 0; row < height; ++row) {
                const std::size_t p = static_cast<std::size_t>(row) * width + c;
                loads_[p] = columns[c]->cores[row];
                pixels_[p] = shade(loads_[p]);
            }
        }
        texture_ = textures_.upload_texture(texture_, width, height, pixels_.data());
        cores_ = height;
    }

    const Fonts &fonts_;
    TextureStore &textures_;
    ImTextureID texture_ = 0;
    std::vector<std::uint32_t> pixels_;
    std::vector<float> loads_;          // the same, as loads, for the hover readout
    std::vector<double> column_t_;      // each column's reading time
    int cores_ = 0;
    double uploaded_t_ = -1;
};

// ------------------------------------------------------------------------------------ busiest core

// The busiest single core: one pinned audio thread makes the audio crackle long before the
// overall CPU looks high.
class BusiestCore final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "busiest"; }
    const char *title() const override { return "Busiest core"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        const auto top = samples.empty() ? std::nullopt : busiest_core(samples.back());
        level_.follow(top ? top->second : 0.0, frame.now);
        // The busiest core over the last two seconds, for "which one keeps peaking".
        std::optional<std::pair<int, float>> recent;
        for (const SystemSample &sample : samples) {
            const auto t = busiest_core(sample);
            if (sample.t >= frame.now - 2.0 && t && (!recent || t->second > recent->second)) recent = t;
        }
        const Box rest = draw_card(draw, box, frame.scale, "Busiest core", percent(top ? std::optional<double>(top->second) : std::nullopt),
                                   top && top->second >= 0.9f ? colour::WARN : colour::TEXT,
                                   {{"core now", top ? std::to_string(top->first) : DASH},
                                    {"peak, 2 s", recent ? "core " + std::to_string(recent->first) + "  " + percent(recent->second) : DASH}});
        const float p = pad(frame.scale);
        if (rest.h > 10 * frame.scale) level_bar(draw, {rest.x + p, rest.y, rest.w - 2 * p, std::min(rest.h - p, 10 * frame.scale)}, level_, frame.scale);
    }

protected:
    int lines() const override { return 3; }

private:
    LevelFollower level_;
};

// ------------------------------------------------------------------------------------ dropouts

// How close the audio blocks come to their deadline, and the blocks that came close enough to drop
// out (the overrun tier): how many, and where the last ones were.
class Dropouts final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "dropouts"; }
    const char *title() const override { return "Dropouts"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const Snapshot &s = frame.snapshot;
        if (!s.ready) {
            draw_card(draw, box, frame.scale, "Dropouts", DASH, colour::TEXT_QUIET, {{"sensor", "not playing"}});
            return;
        }
        const double overrun = s.late_tiers[2];
        // Headroom: how much of the way to the overrun tier the slowest blocks of the last second
        // still have left.
        const std::optional<double> headroom =
            s.late.p99 ? std::optional<double>(std::clamp(1.0 - *s.late.p99 / overrun, 0.0, 1.0)) : std::nullopt;
        std::string last = DASH;
        if (!s.recent_overruns.empty()) {
            const Overrun &o = s.recent_overruns.back();
            char text[48];
            std::snprintf(text, sizeof text, "%s%.0f s ago", o.bar ? ("bar " + std::to_string(*o.bar) + ", ").c_str() : "", o.ago_s);
            last = text;
        }
        draw_card(draw, box, frame.scale, "Dropouts", std::to_string(s.overruns), s.overruns ? colour::WARN : colour::TEXT,
                  {{"headroom", percent(headroom), headroom && *headroom < 0.25 ? colour::WARN : colour::TEXT},
                   {"last", last},
                   {"late p99", percent(s.late.p99)}});
    }

protected:
    int lines() const override { return 3; }
};

// ------------------------------------------------------------------------------------ memory

// The DAW's RAM, the machine's free RAM, and how fast the DAW reads (sample streaming).
class MemoryDisk final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "memory"; }
    const char *title() const override { return "Memory and disk"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        if (samples.empty()) {
            draw_card(draw, box, frame.scale, "Memory and disk", DASH, colour::TEXT_QUIET, {});
            return;
        }
        const SystemSample &now = samples.back();
        const SystemMemory &m = now.memory;
        std::string free = DASH;
        ImU32 free_colour = colour::TEXT;
        if (m.available_bytes && m.total_bytes) {
            free = bytes(m.available_bytes) + " of " + bytes(m.total_bytes);
            if (*m.available_bytes < *m.total_bytes * 0.1) free_colour = colour::WARN;   // close to swapping
        }
        // Disk reads, averaged over the last second: single readings jump.
        double sum = 0;
        int count = 0;
        for (const SystemSample &sample : samples) {
            if (sample.t >= frame.now - 1.0 && sample.daw_read_bytes_per_s) {
                sum += *sample.daw_read_bytes_per_s;
                ++count;
            }
        }
        const std::string reads = count ? bytes(sum / count) + "/s" : DASH;
        draw_card(draw, box, frame.scale, "DAW memory", bytes(now.daw_memory_bytes),
                  now.daw_memory_bytes ? colour::TEXT : colour::TEXT_QUIET,
                  {{"free RAM", free, free_colour}, {"disk reads", reads}});
    }

protected:
    int lines() const override { return 2; }
};

// ------------------------------------------------------------------------------------ clock

// The CPU's speed against its rated speed, and whether the system caps it (a performance limit
// from heat, battery or the power plan): then every block costs more.
class CpuClockCard final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "clock"; }
    const char *title() const override { return "CPU clock"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        const CpuClock::Reading clock = samples.empty() ? CpuClock::Reading{} : samples.back().clock;
        // Slowing down while idle is normal power saving; it only matters when the system caps the
        // speed (the limit), which is what heat, battery and power plans do.
        const bool slow = clock.mhz && clock.rated_mhz && *clock.mhz < *clock.rated_mhz * 0.9;
        const bool limited = clock.limit && *clock.limit < 0.99;
        std::string state = DASH;
        if (clock.mhz) state = limited ? "throttled" : slow ? "power saving" : "full speed";
        draw_card(draw, box, frame.scale, "CPU clock", ghz(clock.mhz), limited ? colour::WARN : colour::TEXT,
                  {{"rated", ghz(clock.rated_mhz)},
                   {"limit", percent(clock.limit), limited ? colour::WARN : colour::TEXT},
                   {"state", state, limited ? colour::WARN : colour::TEXT}});
    }

protected:
    int lines() const override { return 3; }
};

}  // namespace

std::vector<std::unique_ptr<Module>> make_modules(const Fonts &fonts, TextureStore &textures) {
    std::vector<std::unique_ptr<Module>> modules;
    modules.push_back(std::make_unique<PeakMeter>());
    modules.push_back(std::make_unique<CpuNumber>(fonts));
    modules.push_back(std::make_unique<CoreHeatmap>(fonts, textures));
    modules.push_back(std::make_unique<BusiestCore>(fonts));
    modules.push_back(std::make_unique<Dropouts>(fonts));
    modules.push_back(std::make_unique<MemoryDisk>(fonts));
    modules.push_back(std::make_unique<CpuClockCard>(fonts));
    return modules;
}

}  // namespace sessionscope
