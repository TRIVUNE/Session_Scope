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
#include "window/menu.h"
#include "window/renderer.h"

struct GLFWwindow;

namespace sessionscope {

// What the view asks the window to do after a frame (the view itself can't move the window).
struct ViewRequests {
    std::optional<bool> keep_on_top;   // "Keep on top" was clicked: the new value
    std::optional<bool> vertical;      // horizontal/vertical changed: turn the window
    std::optional<int> fps;            // a frame-rate limit was picked: 0, 60 or 30
    std::optional<int> opacity;        // an opacity was picked, in percent
    std::optional<bool> taskbar;       // "Show in taskbar" was clicked: the new value
    std::optional<bool> tray;          // "Tray icon" was clicked: the new value
    bool quit = false;                 // "Quit" was clicked
    // "Pop out" was clicked: this module goes into its own window, of this size (window pixels).
    std::optional<std::string> pop_out;
    ImVec2 pop_out_size = ImVec2(0, 0);
    bool back_in = false;              // a pop-out's "Back into the strip" was clicked
};

// What the desktop side of the HUD looks like now, for the menu's tick marks (window/desktop.h).
struct DesktopState {
    bool available = false;    // Windows: tray icon, hotkey and taskbar choices exist
    bool taskbar = true;
    bool tray = false;
    std::string hotkey;        // "Ctrl+Alt+H", or why it isn't working
};

class HudView {
public:
    // Loads the fonts and makes the modules. `layout_scale` = window pixels per design pixel;
    // `font_oversample` = how much finer to draw letters (2 on a Retina Mac, else 1).
    // `only_module`: for a pop-out window, the one module it shows (no dragging, a short menu).
    // `menu_window`: the window menus belong to (Windows menus need it); nullptr = ImGui menus.
    // Call once, before the first frame, with the window's ImGui context current.
    HudView(TextureStore &textures, double layout_scale, double font_oversample, GLFWwindow *menu_window,
            std::optional<std::string> only_module = std::nullopt);

    // Takes the person's choices (modules and their order, direction, on top) from hud.ini...
    void apply(const HudSettings &settings);
    // ...and writes them back when the HUD closes.
    void remember(HudSettings &settings) const;
    bool vertical() const { return vertical_; }
    // Puts a module back at the end of the strip (its pop-out window was closed).
    void add_module(const std::string &id);
    void set_desktop_state(const DesktopState &state) { desktop_ = state; }
    // The first module's name as people read it ("Busiest core"): a pop-out window's title.
    std::string first_title() const { return modules_.empty() ? std::string() : modules_.front()->title(); }

    // Draws one frame.
    ViewRequests draw(const SystemMonitor &system, const Snapshot &snapshot, double now);

private:
    std::vector<Module *> shown() const;    // the modules shown, in the person's order
    void lay_out(const std::vector<Module *> &modules, ImVec2 window_size);   // fills boxes_
    void drag_modules(ImDrawList *draw);    // picks up, follows and drops a dragged module
    void resize_modules(ImDrawList *draw, const std::vector<Module *> &modules);   // the handles between modules
    // The sizes the person set for the current direction (across or down).
    std::vector<ModuleSize> &sizes() { return vertical_ ? sizes_down_ : sizes_across_; }
    std::optional<int> size_of(const std::string &id) const;   // in design pixels, if set
    void set_size(const std::string &id, float window_pixels);
    void clear_size(const std::string &id);
    ViewRequests menu();                    // the right-click menu
    std::vector<MenuItem> menu_items(const Module *under_mouse) const;   // what the menu offers
    ViewRequests apply_menu_choice(int id, const Module *under_mouse);  // what a picked item does
    float px(float design_pixels) const { return design_pixels * scale_; }

    float scale_;
    Fonts fonts_;
    std::vector<std::unique_ptr<Module>> modules_;   // every module there is, in the default order
    std::vector<std::string> shown_ids_;             // the ids of the modules shown, in order
    std::vector<Box> boxes_;                         // where each shown module is drawn this frame
    std::vector<bool> flexible_;                     // per box: true if it shares the free space
    std::optional<std::size_t> dragging_;            // which shown module is being dragged, if any
    // A resize in progress: the handle after box `handle`, and both boxes' lengths when it began.
    struct Resize {
        std::size_t handle;
        float start_mouse, start_before, start_after;
    };
    std::optional<Resize> resizing_;
    std::vector<ModuleSize> sizes_across_;           // lengths the person set, horizontal layout
    std::vector<ModuleSize> sizes_down_;             // ... vertical layout
    bool vertical_ = false;
    bool on_top_ = true;
    int fps_ = 0;          // frame-rate limit, 0 = the screen's rate
    int opacity_ = 100;    // percent
    GLFWwindow *menu_window_ = nullptr;
    std::optional<std::string> only_module_;    // set: this view is a pop-out window
    DesktopState desktop_;
    const Module *menu_module_ = nullptr;       // the module right-clicked (for an ImGui menu, which stays open)
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_VIEW_H
