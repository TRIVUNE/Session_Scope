#ifndef SESSIONSCOPE_HUD_VIEW_H
#define SESSIONSCOPE_HUD_VIEW_H

// =================================================================================================
// hud_view.h - lays the modules out in a strip, and the right-click menu
// =================================================================================================
//
// Every frame, the view:
//   1. paints the background,
//   2. works out each shown module's box along the strip (left to right, or top to bottom),
//   3. asks each module to draw itself in its box,
//   4. handles dragging a module to a new place,
//   5. shows the right-click menu when asked: which modules to show, horizontal or vertical,
//      keep on top, reset the layout.
//
// The person's choices are saved in hud.ini (engine/src/hud_settings.h) when the HUD closes.
//
// Sizes: modules are designed in "design pixels" (as on a 100 % screen) and multiplied by the
// screen's scale, so the HUD looks the same size on a 4K laptop set to 150 % as on a plain monitor.
//
// Where the idea comes from: MiniMeters' layouts ("Horizontal", "Vertical", modules in an order you
// choose; seen with REA, handoff/minimeters.md).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hud_settings.h"
#include "modules/module.h"
#include "window/renderer.h"

namespace sessionscope {

// What the view asks the window to do after a frame (the view itself can't move the window).
struct ViewRequests {
    std::optional<bool> keep_on_top;   // "Keep on top" was clicked: the new value
    std::optional<bool> vertical;      // horizontal/vertical changed: turn the window
    std::optional<int> fps;            // a frame-rate limit was picked: 0, 60 or 30
    std::optional<int> opacity;        // an opacity was picked, in percent
};

class HudView {
public:
    // Loads the fonts and makes every module. `layout_scale` = window pixels per design pixel;
    // `font_oversample` = how much finer to draw letters (2 on a Retina Mac, else 1).
    // Call once, before the first frame.
    HudView(TextureStore &textures, double layout_scale, double font_oversample);

    // Takes the person's choices (modules and their order, direction, on top) from hud.ini...
    void apply(const HudSettings &settings);
    // ...and writes them back when the HUD closes.
    void remember(HudSettings &settings) const;
    bool vertical() const { return vertical_; }

    // Draws one frame.
    ViewRequests draw(const SystemMonitor &system, const Snapshot &snapshot, double now);

private:
    std::vector<Module *> shown() const;    // the modules shown, in the person's order
    void lay_out(const std::vector<Module *> &modules, ImVec2 window_size);   // fills boxes_
    void drag_modules(ImDrawList *draw);    // picks up, follows and drops a dragged module
    ViewRequests menu();                    // the right-click menu
    float px(float design_pixels) const { return design_pixels * scale_; }

    float scale_;
    Fonts fonts_;
    std::vector<std::unique_ptr<Module>> modules_;   // every module there is, in the default order
    std::vector<std::string> shown_ids_;             // the ids of the modules shown, in order
    std::vector<Box> boxes_;                         // where each shown module is drawn this frame
    std::optional<std::size_t> dragging_;            // which shown module is being dragged, if any
    bool vertical_ = false;
    bool on_top_ = true;
    int fps_ = 0;          // frame-rate limit, 0 = the screen's rate
    int opacity_ = 100;    // percent
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_VIEW_H
