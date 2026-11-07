#ifndef SESSIONSCOPE_HUD_SETTINGS_H
#define SESSIONSCOPE_HUD_SETTINGS_H

// What the HUD remembers between runs: where its window was, how big, which modules it shows
// and whether they run across or down.
// Stored as plain "key=value" lines in hud.ini in the per-user SessionScope folder
// (settings_folder() in trace_folders.h), so a person can read and edit it.
//
// Reading never fails: a missing file, an unknown key or a bad value just leaves that setting at
// its default. Writing is best effort: the HUD works the same if it can't save.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sessionscope {

struct WindowPlace {
    int x = 0, y = 0;            // top-left corner, in screen coordinates
    int width = 0, height = 0;   // in window units (pixels on Windows, points on macOS)
};

struct HudSettings {
    std::optional<WindowPlace> window;
    bool on_top = true;
    // The modules shown, in the order the person dragged them into, by id (engine/hud/modules.h).
    // Unknown ids are ignored.
    std::vector<std::string> modules = {"meter", "cpu", "cores", "busiest", "dropouts", "memory", "clock"};
    bool vertical = false;   // modules stacked top to bottom instead of left to right
};

// <settings folder>/hud.ini, or empty if there is no settings folder.
std::filesystem::path hud_settings_file();

HudSettings load_hud_settings(const std::filesystem::path &file);
bool save_hud_settings(const std::filesystem::path &file, const HudSettings &settings);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_SETTINGS_H
