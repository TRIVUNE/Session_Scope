// =================================================================================================
// draw_helpers.cpp - small pieces every module uses (see draw_helpers.h)
// =================================================================================================

#include "modules/draw_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace sessionscope {

void panel(ImDrawList *draw, const Box &b) {
    draw->AddRectFilled(at(b.x, b.y), at(b.x + b.w, b.y + b.h), colour::PANEL);
    draw->AddRect(at(b.x, b.y), at(b.x + b.w, b.y + b.h), colour::BORDER);
}

// ------------------------------------------------------------------------------------- text

std::string percent(std::optional<double> share) {
    if (!share) return DASH;
    char text[16];
    const double value = *share * 100.0;
    // Under 10 %: one decimal ("3.4%"), so small loads still visibly move. Otherwise whole numbers.
    std::snprintf(text, sizeof text, value < 9.95 ? "%.1f%%" : "%.0f%%", value);
    return text;
}

std::string bytes(std::optional<double> b) {
    if (!b) return DASH;
    char text[24];
    if (*b >= 1e9) std::snprintf(text, sizeof text, "%.1f GB", *b / 1e9);
    else std::snprintf(text, sizeof text, "%.0f MB", *b / 1e6);
    return text;
}

std::string ghz(std::optional<double> mhz) {
    if (!mhz) return DASH;
    char text[24];
    std::snprintf(text, sizeof text, "%.2f GHz", *mhz / 1000.0);
    return text;
}

// ------------------------------------------------------------------------------------- colour

namespace {

// The jet colour scale as "stops": at each load, a colour (red, green, blue, 0..255). Between two
// stops the colour blends smoothly.
struct Stop {
    float at;
    float r, g, b;
};
constexpr std::array<Stop, 7> JET = {{{0.00f, 0, 0, 110},     // dark blue: idle
                                      {0.15f, 0, 20, 255},    // blue
                                      {0.35f, 0, 200, 255},   // cyan
                                      {0.50f, 20, 230, 90},   // green: half busy
                                      {0.65f, 250, 250, 0},   // yellow
                                      {0.82f, 255, 130, 0},   // orange
                                      {1.00f, 225, 0, 0}}};   // red: full

// Works out the colour at one load by blending the two stops around it.
ImU32 blend_jet(float load) {
    std::size_t i = 1;
    while (i + 1 < JET.size() && load > JET[i].at) ++i;   // the first stop at or past `load`
    const Stop &lo = JET[i - 1], &hi = JET[i];
    const float t = (load - lo.at) / (hi.at - lo.at);     // 0 at `lo`, 1 at `hi`
    const auto mix = [t](float a, float b) { return static_cast<int>(std::lround(a + (b - a) * t)); };
    return IM_COL32(mix(lo.r, hi.r), mix(lo.g, hi.g), mix(lo.b, hi.b), 255);
}

// A "lookup table" (LUT): the colour for 256 evenly spaced loads, worked out once when the program
// starts. Looking a colour up is then one array read instead of a loop and some maths: the heatmap
// colours thousands of cells, so this adds up. MiniMeters does the same on the graphics card (a
// "Gradient LUT Texture", seen with REA).
std::array<ImU32, 256> make_jet_table() {
    std::array<ImU32, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) table[i] = blend_jet(static_cast<float>(i) / 255.0f);
    return table;
}
const std::array<ImU32, 256> JET_TABLE = make_jet_table();

}  // namespace

ImU32 jet_colour(float load) {
    const float clamped = std::clamp(load, 0.0f, 1.0f);   // never outside the table
    return JET_TABLE[static_cast<std::size_t>(clamped * 255.0f + 0.5f)];
}

// ------------------------------------------------------------------------------------- readings

std::optional<double> daw_average(const SystemMonitor &system, double now, double seconds) {
    double sum = 0;
    int count = 0;
    // Readings are oldest first, so walk backwards from the newest and stop once they're too old.
    const auto &samples = system.samples();
    for (auto it = samples.rbegin(); it != samples.rend() && it->t >= now - seconds; ++it) {
        if (it->daw) {
            sum += *it->daw;
            ++count;
        }
    }
    if (!count) return std::nullopt;
    return sum / count;
}

std::optional<std::pair<int, float>> busiest_core(const SystemSample &sample) {
    if (sample.cores.empty()) return std::nullopt;
    const auto top = std::max_element(sample.cores.begin(), sample.cores.end());
    return std::make_pair(static_cast<int>(top - sample.cores.begin()), *top);
}

// ------------------------------------------------------------------------------------- meters

void LevelFollower::follow(double target, double now) {
    const double dt = last_ < 0 ? 0 : std::max(0.0, now - last_);   // seconds since the last frame
    last_ = now;
    // Move part of the way towards the target. exp(-dt / settle) is how much of the gap is left
    // after dt seconds; it makes the speed the same at 60 or 144 frames per second.
    const double settle = target > level_ ? 0.04 : 0.45;   // seconds: fast up, slower down
    level_ += (target - level_) * (1.0 - std::exp(-dt / settle));
    if (target >= peak_) {
        peak_ = target;       // a new high: the mark jumps up
        peak_time_ = now;
    } else if (now - peak_time_ > 2.0) {
        peak_ = std::max(target, peak_ - dt * 0.5);   // after 2 s: sinks at half the scale per second
    }
}

float LevelFollower::level() const { return static_cast<float>(std::clamp(level_, 0.0, 1.0)); }
float LevelFollower::peak() const { return static_cast<float>(std::clamp(peak_, 0.0, 1.0)); }

void level_bar(ImDrawList *draw, const Box &box, const LevelFollower &level, float scale) {
    panel(draw, box);
    const Box in = inside(box, 2 * scale);
    const float mark = 2 * scale;   // the peak mark's thickness
    if (box.h >= box.w) {   // tall: fills from the bottom up
        const float bottom = in.y + in.h;
        draw->AddRectFilled(at(in.x, bottom - in.h * level.level()), at(in.x + in.w, bottom), colour::FILL);
        if (level.peak() > 0) {
            const float y = std::max(in.y, bottom - in.h * level.peak() - mark * 0.5f);
            draw->AddRectFilled(at(in.x, y), at(in.x + in.w, y + mark), colour::MARK);
        }
    } else {   // wide: fills from the left
        draw->AddRectFilled(at(in.x, in.y), at(in.x + in.w * level.level(), in.y + in.h), colour::FILL);
        if (level.peak() > 0) {
            const float x = std::min(in.x + in.w - mark, in.x + in.w * level.peak() - mark * 0.5f);
            draw->AddRectFilled(at(x, in.y), at(x + mark, in.y + in.h), colour::MARK);
        }
    }
}

// ------------------------------------------------------------------------------------- cards

float Card::fixed_length(bool vertical, float, float scale) const {
    if (!vertical) return std::round(150 * scale);   // across: 150 design pixels wide
    // Down: tall enough for the title, the big value and every line.
    return std::round(2 * pad(scale) + fonts_.text_size * 1.4f + value_size(scale) * 1.15f +
                      lines() * fonts_.text_size * 1.5f);
}

Box Card::draw_card(ImDrawList *draw, const Box &box, float scale, const char *title, const std::string &value,
                    ImU32 value_colour, const std::vector<CardLine> &card_lines) const {
    panel(draw, box);
    const float p = pad(scale), size = fonts_.text_size;
    const float value_px = std::min(value_size(scale), box.h * 0.36f);   // smaller in a short strip
    float y = box.y + p * 0.75f;   // where the next thing goes, top to bottom

    // Nothing may spill outside the frame: ImGui cuts off ("clips") whatever goes past this rectangle.
    draw->PushClipRect(at(box.x + 1, box.y + 1), at(box.x + box.w - 1, box.y + box.h - 1), true);
    draw->AddText(fonts_.text, size, at(box.x + p, y), colour::TEXT_QUIET, title);
    y += size * 1.4f;
    draw->AddText(fonts_.number, value_px, at(box.x + p - value_px * 0.04f, y), value_colour, value.c_str());
    y += value_px * 1.15f;
    for (const CardLine &line : card_lines) {
        if (y + size > box.y + box.h - p * 0.5f) break;   // no room for another line
        draw->AddText(fonts_.text, size, at(box.x + p, y), colour::TEXT_QUIET, line.label.c_str());
        const float w = text_width(fonts_.text, size, line.value.c_str());
        draw->AddText(fonts_.text, size, at(box.x + box.w - p - w, y), line.colour, line.value.c_str());   // right-aligned
        y += size * 1.5f;
    }
    draw->PopClipRect();
    return {box.x, y, box.w, box.y + box.h - y};
}

}  // namespace sessionscope
