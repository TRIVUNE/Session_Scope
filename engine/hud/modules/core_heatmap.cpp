// =================================================================================================
// core_heatmap.cpp - module "cores": every core of the computer over time, with the song's bars
// =================================================================================================
//
// What you see:
//   - One row per core (core 0 at the top), numbered on the left.
//   - Time runs left to right; the right edge is "now", the left edge 12 seconds ago.
//   - Each cell's colour says how busy that core was: dark blue idle ... red full (jet colours).
//   - Where a new bar of the song starts, a thin line with the bar number in a ruler on top, like
//     the timeline of a DAW.
//   - Hover the mouse over a cell for the exact number.
//
// How it stays smooth: readings arrive only ten times a second, but the screen shows 60-144 frames
// a second. If we redrew the picture only when a reading arrived it would jump in steps. Instead,
// each reading becomes one column of pixels in a picture (a "texture") on the graphics card, and
// every frame we draw that picture shifted left by the time that has passed. So it slides a tiny
// bit every frame, and the graphics card does the stretching for free.
//
// Cost: when a reading arrives (10 times a second) we rebuild the picture: about 120 columns x 16
// cores = ~2,000 pixels, each one array lookup (jet_colour). That's a few microseconds. Each frame
// in between only draws one picture and a few lines.
//
// Where it comes from:
//   - Per-core readings: CoreCpu (engine/src/core_cpu.h), the same Windows source as Task Manager.
//   - Values in a texture, coloured by a table: MiniMeters draws its spectrogram this way (seen
//     with REA: a "lut_tex" shader and "Gradient LUT Texture").
//   - Bar numbers: from the sensor plug-in via the engine (snapshot.bar), recorded with each reading.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class CoreHeatmap final : public Module {
public:
    CoreHeatmap(const Fonts &fonts, TextureStore &textures) : fonts_(fonts), textures_(textures) {}
    ~CoreHeatmap() override {
        if (texture_) textures_.destroy_texture(texture_);
    }
    const char *id() const override { return "cores"; }
    const char *title() const override { return "Core heatmap"; }
    float fixed_length(bool, float, float) const override { return 0; }   // takes the free space
    float weight() const override { return 3; }                           // three times as much as others

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        // ---- the layout: a ruler on top (bar numbers), a gutter on the left (core numbers), the map
        panel(draw, box);
        const Box in = inside(box, 1);
        const float size = fonts_.text_size;
        const float ruler = std::round(size + 6 * frame.scale);
        const float gutter = std::round(text_width(fonts_.text, size, "00") + 8 * frame.scale);
        const Box map = {in.x + gutter, in.y + ruler, in.w - gutter, in.h - ruler};
        draw->AddLine(at(in.x, map.y - 0.5f), at(in.x + in.w, map.y - 0.5f), colour::FILL);   // under the ruler
        draw->AddLine(at(map.x - 0.5f, in.y), at(map.x - 0.5f, in.y + in.h), colour::FILL);   // beside the gutter
        draw->AddRectFilled(at(map.x, map.y), at(map.x + map.w, map.y + map.h), jet_colour(0));   // idle until data comes
        draw->AddText(fonts_.text, size, at(in.x + 4 * frame.scale, in.y + (ruler - size) * 0.5f), colour::TEXT_QUIET, "core");

        const auto &samples = frame.system.samples();
        rebuild_picture_if_new_reading(samples);
        if (cores_ == 0) return;   // no readings yet

        // ---- core numbers: every row if they fit, else every 2nd, 4th... row
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

        // ---- where a moment in time lands on screen
        // `step` = seconds between readings (about 0.1). We show the picture one step late: the
        // newest column is still sliding in from the right as the next reading arrives, so the
        // picture never jumps.
        const double step = std::max(0.02, (samples.back().t - samples.front().t) / (samples.size() - 1));
        const double shown_until = frame.now - step;   // the moment at the right edge
        const double span = frame.system.span();       // seconds across the whole map (12)
        const auto x_of = [&](double t) { return static_cast<float>(map.x + map.w - (shown_until - t) / span * map.w); };

        // ---- the picture, and the bar lines over it, cut off at the map's edges
        draw->PushClipRect(at(map.x, in.y), at(in.x + in.w, in.y + in.h), true);
        draw->AddImage(texture_, at(x_of(column_t_.front() - step), map.y), at(x_of(column_t_.back()), map.y + map.h));
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const std::optional<int> bar = samples[i].bar;
            if (!bar || samples[i - 1].bar == bar) continue;   // a bar line where the bar number changes
            const float x = std::round(x_of(samples[i].t - step)) + 0.5f;   // +0.5: a crisp 1-pixel line
            draw->AddLine(at(x, in.y), at(x, map.y), colour::MARK);                            // black in the ruler
            draw->AddLine(at(x, map.y), at(x, map.y + map.h), IM_COL32(255, 255, 255, 200));   // white over the colours
            const std::string label = std::to_string(*bar);
            draw->AddText(fonts_.text, size, at(x + 3 * frame.scale, in.y + (ruler - size) * 0.5f), colour::TEXT,
                          label.c_str());
        }
        draw->PopClipRect();

        show_hover(draw, map, row_h, shown_until, span, frame.now);
    }

private:
    // Rebuilds the picture when a new reading has come: one column per reading, one row per core.
    void rebuild_picture_if_new_reading(const std::deque<SystemSample> &samples) {
        if (samples.empty() || samples.back().t == uploaded_t_) return;   // nothing new: keep the picture
        uploaded_t_ = samples.back().t;
        const std::size_t cores = samples.back().cores.size();
        if (!cores) return;

        // The readings that have every core (the very first reading has none: no baseline yet).
        column_t_.clear();
        columns_.clear();
        for (const SystemSample &sample : samples) {
            if (sample.cores.size() == cores) {
                columns_.push_back(&sample);
                column_t_.push_back(sample.t);
            }
        }
        const int width = static_cast<int>(columns_.size()), height = static_cast<int>(cores);
        // assign() reuses the vectors' memory from last time, so this doesn't ask for new memory.
        pixels_.assign(static_cast<std::size_t>(width) * height, 0);
        loads_.assign(pixels_.size(), 0);
        for (int c = 0; c < width; ++c) {
            for (int row = 0; row < height; ++row) {
                const std::size_t p = static_cast<std::size_t>(row) * width + c;   // row by row, left to right
                loads_[p] = columns_[c]->cores[row];
                pixels_[p] = jet_colour(loads_[p]);
            }
        }
        texture_ = textures_.upload_texture(texture_, width, height, pixels_.data());
        cores_ = height;
    }

    // Mouse over the map: outline that core's row and show "Core 3  45%  2.1 s ago".
    void show_hover(ImDrawList *draw, const Box &map, float row_h, double shown_until, double span, double now) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool over_map = mouse.x >= map.x && mouse.x < map.x + map.w && mouse.y >= map.y && mouse.y < map.y + map.h;
        if (!over_map || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            return;
        }
        const int core = std::clamp(static_cast<int>((mouse.y - map.y) / row_h), 0, cores_ - 1);
        const double t = shown_until - (map.x + map.w - mouse.x) / map.w * span;   // the moment under the mouse
        // The first column at or after t is the one drawn there (each column covers the step before it).
        const auto later = std::lower_bound(column_t_.begin(), column_t_.end(), t);
        if (later == column_t_.end()) return;
        const std::size_t column = static_cast<std::size_t>(later - column_t_.begin());
        const float load = loads_[static_cast<std::size_t>(core) * column_t_.size() + column];
        draw->AddRect(at(map.x, map.y + row_h * core), at(map.x + map.w, map.y + row_h * (core + 1)), IM_COL32(255, 255, 255, 160));
        ImGui::BeginTooltip();
        ImGui::Text("Core %d   %s", core, percent(load).c_str());
        ImGui::TextDisabled("%.1f s ago", std::max(0.0, now - column_t_[column]));
        ImGui::EndTooltip();
    }

    const Fonts &fonts_;
    TextureStore &textures_;
    ImTextureID texture_ = 0;                    // the picture on the graphics card
    std::vector<const SystemSample *> columns_;  // the readings in the picture, oldest first
    std::vector<std::uint32_t> pixels_;          // the picture's colours, kept to reuse the memory
    std::vector<float> loads_;                   // the same cells as loads, for the hover text
    std::vector<double> column_t_;               // each column's reading time
    int cores_ = 0;
    double uploaded_t_ = -1;                     // the newest reading in the picture
};

}  // namespace

std::unique_ptr<Module> make_core_heatmap(const Fonts &fonts, TextureStore &textures) {
    return std::make_unique<CoreHeatmap>(fonts, textures);
}

}  // namespace sessionscope
