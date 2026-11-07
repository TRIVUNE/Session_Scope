#ifndef SESSIONSCOPE_HUD_VIEW_H
#define SESSIONSCOPE_HUD_VIEW_H

// Draws the HUD: the chosen modules (modules.h) side by side, left to right, or stacked top to
// bottom. Right-click anywhere for the menu: which modules to show, which direction they run,
// and whether the window stays on top. The choices are saved in hud.ini.
//
// Sizes are written in CSS pixels and multiplied by the display scale.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hud_settings.h"
#include "modules.h"

namespace sessionscope {

// What the view asks of the window after a frame.
struct ViewRequests {
    std::optional<bool> keep_on_top;   // "Keep on top" was clicked
    std::optional<bool> vertical;      // the direction changed: turn the window
};

class HudView {
public:
    // Loads the fonts and makes the modules. `layout_scale` turns CSS pixels into window units;
    // `font_oversample` rasterises text that much finer (2 on a Retina Mac). Call once, before
    // the first frame.
    HudView(TextureStore &textures, double layout_scale, double font_oversample);

    // The choices the view owns (modules shown, direction, on top): taken from the saved settings
    // at start, written back into them at the end.
    void apply(const HudSettings &settings);
    void remember(HudSettings &settings) const;
    bool vertical() const { return vertical_; }

    // Draws one display frame.
    ViewRequests draw(const CpuMonitor &cpu, const Snapshot &snapshot, double now);

private:
    // The modules shown, in order.
    std::vector<Module *> shown() const;
    ViewRequests menu();
    float px(float css_pixels) const { return css_pixels * scale_; }

    float scale_;
    Fonts fonts_;
    std::vector<std::unique_ptr<Module>> modules_;   // every module, in the default order
    std::vector<std::string> shown_ids_;
    bool vertical_ = false;
    bool on_top_ = true;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_VIEW_H
