#ifndef SESSIONSCOPE_HUD_SETTINGS_H
#define SESSIONSCOPE_HUD_SETTINGS_H

// =================================================================================================
// hud_settings.h - what the HUD remembers between runs (hud.ini)
// =================================================================================================
//
// Stored as plain "key=value" lines in hud.ini, in the per-user SessionScope folder
// (settings_folder() in trace_folders.h: %APPDATA%\SessionScope on Windows), so a person can read
// and edit it. For example:
//
//     window=120,80,1400,150             x, y, width, height of the main window
//     on_top=yes
//     modules=meter,cpu,cores,busiest    the modules in the strip, in order
//     vertical=no
//     sizes_across=cores:640,busiest:160 lengths the person dragged, in design pixels
//     sizes_down=cores:300
//     fps=0                              frame-rate limit: 0 = the screen's rate, or 60, 30
//     opacity=100                        how solid the windows are, in percent (25-100)
//     tray=no                            an icon in the taskbar's notification area
//     taskbar=yes                        a button in the taskbar
//     hotkey=ctrl+alt+h                  shows / hides the HUD from anywhere
//     popout=dropouts@1500,80,180,160    a module in its own window (one line each)
//
// Reading never fails: a missing file, an unknown key or a bad value leaves that setting at its
// default. Writing is best effort: the HUD works the same if it can't save.

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sessionscope {

// Where a window is and how big.
struct WindowPlace {
    int x = 0, y = 0;            // top-left corner, in screen coordinates
    int width = 0, height = 0;   // in window units (pixels on Windows, points on macOS)
};

// A module the person popped out into its own window.
struct PopOut {
    std::string module;   // its id, like "dropouts"
    WindowPlace place;
};

// A module's length along the strip that the person set by dragging, in design pixels.
using ModuleSize = std::pair<std::string, int>;   // (module id, length)

struct HudSettings {
    std::optional<WindowPlace> window;
    bool on_top = true;
    // The modules in the strip, in the order the person dragged them into, by id
    // (engine/hud/modules/module.h). Unknown ids are ignored.
    std::vector<std::string> modules = {"meter", "cpu", "cores", "tasks", "busiest", "dropouts", "memory", "clock"};
    bool vertical = false;                 // modules stacked top to bottom instead of left to right
    std::vector<ModuleSize> sizes_across;  // lengths set by dragging, in the horizontal layout
    std::vector<ModuleSize> sizes_down;    // ... and in the vertical one
    int fps = 0;                           // frame-rate limit: 0 = as fast as the screen refreshes
    int opacity = 100;                     // percent, 25 to 100
    bool tray = false;                     // icon in the notification area (Windows)
    bool taskbar = true;                   // button in the taskbar
    std::string hotkey = "ctrl+alt+h";     // shows / hides every HUD window
    std::vector<PopOut> popouts;           // modules in their own windows
};

// <settings folder>/hud.ini, or empty if there is no settings folder.
std::filesystem::path hud_settings_file();

HudSettings load_hud_settings(const std::filesystem::path &file);
bool save_hud_settings(const std::filesystem::path &file, const HudSettings &settings);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_SETTINGS_H
