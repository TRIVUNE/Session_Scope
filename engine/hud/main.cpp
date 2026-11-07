// =================================================================================================
// main.cpp - where the HUD program starts
// =================================================================================================
//
// Builds into build/engine/SessionScopeHUD.exe (tools/build_engine.py).
//
//   SessionScopeHUD                      live: shows the DAW where the sensor plug-in plays
//   SessionScopeHUD --demo               generated data, no DAW needed
//   SessionScopeHUD --replay FOLDER      plays back a recording
//   add --screenshot OUT.png [--wait MS] [--vertical]  to save one frame after MS milliseconds and quit
//   add --test-drag X1,Y1,X2,Y2 to a screenshot run to play a mouse drag in (for testing)
//
// This file only reads the command line; hud_app.cpp does the rest.
//
// Map of the HUD's code (engine/hud/):
//   main.cpp, hud_app.*     start, options, the main loop
//   hud_view.*              lays the modules out, drag and drop, right-click menu
//   modules/                one file per module, plus shared drawing helpers
//   window/                 the window (GLFW) and drawing on the graphics card (Direct3D / OpenGL)
//   look/                   colours and fonts
// The data comes from engine/src/: hud_engine.* (sensor data) and system_monitor.* (the computer).

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "hud_app.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#endif

namespace {

using sessionscope::HudOptions;

// Turns the command line's words into HudOptions. Unknown words stop the program with a message.
HudOptions parse_args(int argc, char **argv) {
    HudOptions options;
    for (int i = 1; i < argc; ++i) {   // argv[0] is the program's own name
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;   // is there another word after this one?
        if (arg == "--replay" && has_value) options.replay_path = argv[++i];
        else if (arg == "--demo") options.demo = true;
        else if (arg == "--screenshot" && has_value) options.screenshot = argv[++i];
        else if (arg == "--wait" && has_value) options.wait_ms = std::atoi(argv[++i]);
        else if (arg == "--vertical") options.vertical = true;
        else if (arg == "--test-drag" && has_value) {
            // Four numbers separated by commas: "300,60,900,60".
            std::array<float, 4> drag{};
            const char *text = argv[++i];
            char *end = nullptr;
            for (float &number : drag) {
                number = std::strtof(text, &end);
                text = (*end == ',') ? end + 1 : end;
            }
            options.test_drag = drag;
        }
        else {
            sessionscope::show_error("Unknown or incomplete argument: " + arg +
                                     "\n\nUsage: SessionScopeHUD [--replay FOLDER] [--demo] [--vertical] "
                                     "[--screenshot OUT.png] [--wait MS] [--test-drag X1,Y1,X2,Y2]");
            std::exit(2);
        }
    }
    return options;
}

}  // namespace

#ifdef _WIN32
// A Windows program without a console window starts at WinMain instead of main. Windows gives us
// the command line as one UTF-16 string; we split it into words and convert each to UTF-8, so file
// paths with any characters work.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR *wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv) return 1;

    std::vector<std::string> words;
    words.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        // First ask how many bytes the UTF-8 version needs, then convert.
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string word(static_cast<std::size_t>(bytes > 0 ? bytes - 1 : 0), '\0');   // -1: no room for the final zero
        WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, word.data(), bytes, nullptr, nullptr);
        words.push_back(word);
    }
    LocalFree(wide_argv);

    std::vector<char *> argv;   // main-style: a list of pointers to the words
    for (auto &word : words) argv.push_back(word.data());
    return sessionscope::run_hud(parse_args(argc, argv.data()));
}
#else
int main(int argc, char **argv) { return sessionscope::run_hud(parse_args(argc, argv)); }
#endif
