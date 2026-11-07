#ifndef SESSIONSCOPE_HUD_VIEW_H
#define SESSIONSCOPE_HUD_VIEW_H

// Draws the HUD from engine snapshots. The engine does all measuring and statistics; this only
// draws.
//
// Look: in the MiniMeters spirit. Black, edge to edge, no panels; colour is in the data only (the
// inferno palette: black, purple, magenta, orange, pale yellow); axes are small grey ticks. Top: the
// lateness spectrogram with its profile, the bar map strip, the waveform and the readout. Below: the
// Task Manager list of bars, which can be folded away.
//
// Sizes are written in the old web HUD's CSS pixels and multiplied by the display scale, so the two
// look the same on any screen.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hud_engine.h"
#include "hud_settings.h"
#include "renderer.h"

struct ImFont;
struct ImDrawList;

namespace sessionscope {

// What the view asks of the window after a frame.
struct ViewRequests {
    std::optional<bool> keep_on_top;   // the pin was clicked
    std::optional<bool> compact;       // "compact" or "full" was clicked: resize the window
};

class HudView {
public:
    // Loads fonts. `layout_scale` turns the web HUD's CSS pixels into window units;
    // `font_oversample` rasterises text that much finer (2 on a Retina Mac). Call once, before
    // the first frame.
    HudView(TextureStore &textures, double layout_scale, double font_oversample);
    ~HudView();
    HudView(const HudView &) = delete;
    HudView &operator=(const HudView &) = delete;

    // A new snapshot from the engine: keep its new spectrogram columns. Call once per snapshot.
    void receive(const Snapshot &snapshot);

    // Draws one display frame. `sensor_installed` feeds the waiting checklist.
    ViewRequests draw(bool sensor_installed);

    // The settings the view owns (pin, list, sort column, compact mode): taken from the saved
    // settings at start, written back into them at the end.
    void apply(const HudSettings &settings);
    void remember(HudSettings &settings) const;
    bool compact() const { return compact_; }
    // The bar whose row was clicked open in the list, for HudEngine::select_bar.
    std::optional<int> selected_bar() const { return selected_bar_; }

    // True while something moves on screen by itself (the spectrogram slides while the host
    // plays). When false, the window only needs redrawing for new data or the mouse.
    bool animating() const { return have_state_ && state_.playing; }

private:
    struct Rect {
        float x, y, w, h;
    };
    struct Layout {
        Rect spec, prof, bars, bars_right, wave, read, list;
    };

    Layout layout(float width, float height) const;
    void draw_spectrogram(ImDrawList *draw, const Rect &r);
    void draw_spectrogram_axes(ImDrawList *draw, const Rect &r);
    void draw_spectrogram_hover(ImDrawList *draw, const Rect &r);
    void draw_waiting_checklist(ImDrawList *draw, const Rect &r, bool sensor_installed);
    void draw_profile(ImDrawList *draw, const Rect &r);
    void draw_transport(ImDrawList *draw, const Rect &r);
    void draw_bar_map(ImDrawList *draw, const Rect &r, const Rect &right);
    void draw_waveform(ImDrawList *draw, const Rect &r);
    void draw_readout(ImDrawList *draw, const Rect &r, ViewRequests &requests);
    void draw_level_meter(ImDrawList *draw, const Rect &r);
    void draw_task_manager(const Rect &r);
    void draw_bar_detail(ImDrawList *rows, const Rect &r, float indent, const Metric &metric);

    void rebuild_spectrogram_texture();
    float px(float css_pixels) const { return css_pixels * scale_; }

    float scale_;
    ImFont *mono9_ = nullptr;
    ImFont *mono10_ = nullptr;
    ImFont *mono11_ = nullptr;
    ImFont *mono13_ = nullptr;
    ImFont *ui13_ = nullptr;
    ImFont *ui15_ = nullptr;
    ImFont *ui17_ = nullptr;

    Snapshot state_;
    bool have_state_ = false;

    // Spectrogram: column id -> bin counts (empty = silent), only what is on screen.
    std::map<int, std::vector<int>> columns_;
    std::optional<int> newest_column_;
    double newest_arrived_at_ = 0;       // when the newest column came, for the smooth slide
    TextureStore &textures_;
    ImTextureID texture_ = 0;            // texture of the visible columns
    bool texture_dirty_ = true;

    double peak_value_ = 0;              // the level meter's 3-second peak hold
    double peak_time_ = 0;

    bool on_top_ = true;
    bool list_open_ = true;
    bool compact_ = false;   // meter only: the waveform and the readout, nothing else
    std::optional<int> selected_bar_;   // the bar opened in the list: one line per pass below it

    // The Task Manager list.
    std::string sort_key_ = "bar";
    std::map<std::string, bool> group_open_ = {{"now", true}, {"hot", true}, {"all", true}};
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_VIEW_H
