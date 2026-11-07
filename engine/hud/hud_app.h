#ifndef SESSIONSCOPE_HUD_APP_H
#define SESSIONSCOPE_HUD_APP_H

// =================================================================================================
// hud_app.h - runs the HUD from start to finish
// =================================================================================================
//
// main.cpp reads the command line into HudOptions and calls run_hud(), which:
//   1. loads the saved settings (hud.ini) and makes the window,
//   2. starts the engine (live sensor, a replayed recording, or the demo) and the SystemMonitor,
//   3. loops until the window is closed:
//        ten times a second: the engine reads new sensor data, the SystemMonitor reads the computer
//        every screen refresh: the view draws a frame
//   4. saves the settings again.

#include <filesystem>
#include <optional>
#include <string>

namespace sessionscope {

// What the command line asked for.
struct HudOptions {
    std::string replay_path;                          // --replay FOLDER: play back a recording
    bool demo = false;                                // --demo: generated data, no DAW needed
    std::optional<std::filesystem::path> screenshot;  // --screenshot FILE.png: save one frame and quit
    int wait_ms = 4000;                               // --wait MS: how long before that screenshot
    bool vertical = false;                            // --vertical: modules top to bottom (for screenshots)
};

// Runs the HUD; returns the program's exit code (0 = fine).
int run_hud(const HudOptions &options);

// Shows an error where the person can see it: a message box on Windows (a windowed program has no
// console), the terminal elsewhere.
void show_error(const std::string &message);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_APP_H
