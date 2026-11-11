// =================================================================================================
// task_manager.cpp - module "tasks": CPU per track and per plug-in, like Windows' Task Manager
// =================================================================================================
//
// What you see: one row per track (in the DAW's order), with the CPU its plug-ins use together and
// its worst block in the last second. Click a track to open it and see each plug-in. Click the
// "CPU" title to sort by CPU instead (busiest first), "Track" to go back to the DAW's order.
// When there are more rows than fit, turn the mouse wheel over the module to scroll; a thin bar at
// the right edge shows where you are.
//
// What "CPU" means: the share of each audio block's time spent inside the plug-in. 25 % = a quarter
// of the time the DAW has for a whole block. The tracks together add up to the DAW's audio load;
// near 100 % the audio drops out. "Peak" is the worst single block in the last second.
//
// Where the numbers come from: SessionScope Probes, a wrapper plug-in that loads the real plug-in
// and times it (handoff/task-manager-plan.md), through ProbeMonitor (engine/src/probe_monitor.h).
// The person asked for this module: "the cpu in the task manager should be per track, each track
// having its elements, and then it displays what cpu each is using". Its look follows Windows'
// Task Manager (rows that open up, numbers right-aligned) in the HUD's light theme.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class TaskManager final : public Module {
public:
    explicit TaskManager(const Fonts &fonts) : fonts_(fonts) {}
    const char *id() const override { return "tasks"; }
    const char *title() const override { return "Task Manager (CPU per track)"; }
    float fixed_length(bool, float, float) const override { return 0; }   // shares the free space
    float weight() const override { return 2; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        panel(draw, box);
        const float s = frame.scale, size = fonts_.text_size;
        const float row_h = std::round(size * 1.7f), pad = 8 * s;
        const Box in = inside(box, 1);

        // ---- columns: name on the left, then CPU (a small bar and the number) and Peak, right-aligned
        // CPU is as wide as "100%" plus a small bar, the bar only when the module is wide enough
        // that names still fit (names matter more than bars).
        const float number_w = text_width(fonts_.text, size, "100%") + pad * 1.5f;
        const float peak_w = std::round(number_w);
        const float cpu_w = std::round(number_w + (in.w > 300 * s ? 50 * s : 0));
        const float name_w = std::max(0.0f, in.w - cpu_w - peak_w);
        const float cpu_x = in.x + name_w, peak_x = cpu_x + cpu_w;

        // ---- header: the column titles; clicking them sorts
        header(draw, in.x, in.y, name_w, row_h, pad, "Track", !by_cpu_, false);
        if (clicked(in.x, in.y, name_w, row_h)) by_cpu_ = false;
        header(draw, cpu_x, in.y, cpu_w, row_h, pad, "CPU", by_cpu_, true);
        if (clicked(cpu_x, in.y, cpu_w, row_h)) by_cpu_ = true;
        header(draw, peak_x, in.y, peak_w, row_h, pad, "Peak", false, true);
        draw->AddLine(at(in.x, in.y + row_h - 0.5f), at(in.x + in.w, in.y + row_h - 0.5f), colour::FILL);

        std::vector<TrackLoad> tracks = frame.probes.tracks();
        if (tracks.empty()) {   // no probe running yet: say how to get some
            const char *lines[] = {"No probes running.", "Put SessionScope Probe on a track and load", "a plug-in in it to see its CPU here."};
            float y = in.y + row_h + pad;
            for (const char *line : lines) {
                draw->AddText(fonts_.text, size, at(in.x + pad, y), colour::TEXT_QUIET, line);
                y += size * 1.4f;
            }
            return;
        }
        if (by_cpu_) {
            std::stable_sort(tracks.begin(), tracks.end(), [](const TrackLoad &a, const TrackLoad &b) { return a.cpu > b.cpu; });
            for (TrackLoad &t : tracks) {
                std::stable_sort(t.plugins.begin(), t.plugins.end(), [](const PluginLoad &a, const PluginLoad &b) { return a.cpu > b.cpu; });
            }
        }

        // ---- scrolling: how tall all rows are together, against the room under the header
        std::size_t row_count = tracks.size();
        for (const TrackLoad &track : tracks) {
            if (open_.count(track.name)) row_count += track.plugins.size();   // an open track shows its plug-ins too
        }
        const float top = in.y + row_h, visible_h = std::max(0.0f, in.h - row_h);
        const float content_h = static_cast<float>(row_count) * row_h;
        const float max_scroll = std::max(0.0f, content_h - visible_h);       // 0: everything fits
        const ImGuiIO &io = ImGui::GetIO();
        const bool mouse_over = io.MousePos.x >= box.x && io.MousePos.x < box.x + box.w &&
                                io.MousePos.y >= box.y && io.MousePos.y < box.y + box.h;
        // One notch of the wheel moves two rows. MouseWheel is positive when turned away from you
        // (content should move down, so we scroll back up).
        if (mouse_over && io.MouseWheel != 0) scroll_ -= io.MouseWheel * row_h * 2;
        scroll_ = std::clamp(scroll_, 0.0f, max_scroll);                      // also pulls back when rows disappear

        // ---- rows, drawn only inside the room under the header
        const float bottom = in.y + in.h;
        const auto in_view = [&](float row_y) { return row_y + row_h > top && row_y < bottom; };   // any part of the row shows
        draw->PushClipRect(at(in.x, top), at(in.x + in.w, bottom), true);
        float y = top - std::round(scroll_);   // where the first row starts: above `top` once scrolled
        for (const TrackLoad &track : tracks) {
            if (y >= bottom) break;            // everything from here on is below the view
            const bool open = open_.count(track.name) > 0;
            if (in_view(y)) {
                // A small triangle: pointing down when open, right when closed. (Drawn, not typed: the
                // Inter font has no arrow characters.)
                const float cx = in.x + pad + 4 * s, cy = y + row_h * 0.5f, t = 3.5f * s;
                if (open) draw->AddTriangleFilled(at(cx - t, cy - t * 0.6f), at(cx + t, cy - t * 0.6f), at(cx, cy + t * 0.8f), colour::TEXT);
                else draw->AddTriangleFilled(at(cx - t * 0.6f, cy - t), at(cx + t * 0.8f, cy), at(cx - t * 0.6f, cy + t), colour::TEXT);
                row(draw, in.x + 12 * s, y, row_h, pad, track.name, colour::TEXT, track.cpu, track.peak, cpu_x, cpu_w, peak_x, peak_w);
                // Open or close the track. Only the part of the row that shows can be clicked: the
                // part scrolled under the header belongs to the header.
                const float click_y = std::max(y, top);
                if (clicked(in.x, click_y, in.w, y + row_h - click_y)) {
                    if (open) open_.erase(track.name);
                    else open_.insert(track.name);
                }
            }
            y += row_h;
            if (!open) continue;
            for (const PluginLoad &plugin : track.plugins) {
                if (y >= bottom) break;
                if (in_view(y)) {
                    row(draw, in.x + 26 * s, y, row_h, pad, plugin.name, colour::TEXT_QUIET, plugin.cpu, plugin.peak,
                        cpu_x, cpu_w, peak_x, peak_w);
                }
                y += row_h;
            }
        }
        draw->PopClipRect();

        // ---- the scroll bar: only when there is something to scroll. A thin grey track with a
        // darker piece whose length says how much of the list is in view, and whose place says where.
        if (max_scroll > 0 && visible_h > 0) {
            const float bar_w = std::max(2.0f, std::round(3 * s)), bar_x = in.x + in.w - bar_w;
            const float thumb_h = std::max(12 * s, visible_h * visible_h / content_h);
            const float thumb_y = top + (visible_h - thumb_h) * (scroll_ / max_scroll);
            draw->AddRectFilled(at(bar_x, top), at(bar_x + bar_w, top + visible_h), colour::HOVER);
            draw->AddRectFilled(at(bar_x, thumb_y), at(bar_x + bar_w, thumb_y + thumb_h), colour::TEXT_QUIET);
        }
    }

private:
    // A column title; the sorted column is dark, the others grey.
    void header(ImDrawList *draw, float x, float y, float w, float h, float pad, const char *text, bool sorted, bool right) const {
        const float size = fonts_.text_size;
        const float tx = right ? x + w - pad - text_width(fonts_.text, size, text) : x + pad;
        draw->AddText(fonts_.text, size, at(tx, y + (h - size) * 0.5f), sorted ? colour::TEXT : colour::TEXT_QUIET, text);
    }

    // One row: the name (cut off before the CPU column), a small CPU bar with its number, the peak.
    void row(ImDrawList *draw, float x, float y, float h, float pad, const std::string &name, ImU32 name_colour,
             double cpu, double peak, float cpu_x, float cpu_w, float peak_x, float peak_w) const {
        const float size = fonts_.text_size, text_y = y + (h - size) * 0.5f;
        draw->PushClipRect(at(x, y), at(cpu_x - 4, y + h), true);
        draw->AddText(fonts_.text, size, at(x + pad, text_y), name_colour, name.c_str());
        draw->PopClipRect();

        // The bar: the CPU share of the block time, grey, under the number.
        const float bar_x = cpu_x + pad * 0.5f, bar_w = cpu_w - pad * 1.5f - text_width(fonts_.text, size, "100%");
        const float bar_y = y + h * 0.3f, bar_h = h * 0.4f;
        if (bar_w > 4) {
            draw->AddRectFilled(at(bar_x, bar_y), at(bar_x + bar_w * static_cast<float>(std::clamp(cpu, 0.0, 1.0)), bar_y + bar_h), colour::FILL);
        }
        // Red from 70 % of the block (getting close to a dropout), else normal.
        const std::string cpu_text = percent(cpu), peak_text = percent(peak);
        draw->AddText(fonts_.text, size, at(cpu_x + cpu_w - pad - text_width(fonts_.text, size, cpu_text.c_str()), text_y),
                      cpu >= 0.7 ? colour::WARN : colour::TEXT, cpu_text.c_str());
        draw->AddText(fonts_.text, size, at(peak_x + peak_w - pad - text_width(fonts_.text, size, peak_text.c_str()), text_y),
                      peak >= 0.9 ? colour::WARN : colour::TEXT_QUIET, peak_text.c_str());
    }

    // True on the frame the left button is let go over this rectangle without having been dragged
    // (a drag moves the module instead, hud_view.cpp).
    static bool clicked(float x, float y, float w, float h) {
        const ImGuiIO &io = ImGui::GetIO();
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || io.MouseDragMaxDistanceSqr[0] > 25.0f) return false;
        const ImVec2 m = io.MousePos;
        return m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
    }

    const Fonts &fonts_;
    bool by_cpu_ = false;               // sort by CPU instead of the DAW's order
    float scroll_ = 0;                  // how far the rows are scrolled up, in pixels (0 = the top)
    std::set<std::string> open_;        // tracks opened to show their plug-ins
};

}  // namespace

std::unique_ptr<Module> make_task_manager(const Fonts &fonts) { return std::make_unique<TaskManager>(fonts); }

}  // namespace sessionscope
