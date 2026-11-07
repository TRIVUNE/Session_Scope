// SessionScope HUD native entry point.
// Builds build/engine/SessionScopeHUD.exe
// Options: --replay TRACE | --demo [--screenshot OUT.png] [--wait MS]
// Draws the HUD (hud_view.h) from engine snapshots.

#include "hud_window.h"
#include "hud_engine.h"
#include "hud_settings.h"
#include "hud_view.h"
#include "simulate.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace sessionscope {

struct Options {
    std::string replay_path;
    bool demo = false;
    std::optional<std::filesystem::path> screenshot;
    int wait_ms = 4000;
};

static void show_error(const std::string &msg);

static Options parse_args(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--replay" && i + 1 < argc) {
            opt.replay_path = argv[++i];
        } else if (arg == "--demo") {
            opt.demo = true;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opt.screenshot = argv[++i];
        } else if (arg == "--wait" && i + 1 < argc) {
            opt.wait_ms = std::atoi(argv[++i]);
        } else {
            // A windowed program has no console, so say it where it can be seen.
            show_error("Unknown or incomplete argument: " + arg +
                       "\n\nUsage: SessionScopeHUD [--replay TRACE] [--demo] [--screenshot OUT.png] [--wait MS]");
            std::exit(2);
        }
    }
    return opt;
}

static std::unique_ptr<HudEngine> make_engine(const Options &opt, std::string &note) {
    using namespace std::chrono;

    if (!opt.replay_path.empty()) {
        auto sensor = std::make_unique<ReplaySensor>(opt.replay_path);
        auto host = std::make_unique<ReplayHost>(*static_cast<ReplaySensor *>(sensor.get()));
        return std::make_unique<HudEngine>(std::move(sensor), std::move(host), "replay", std::nullopt);
    }

    if (opt.demo) {
        // A simulated song, replayed: the HUD without any DAW. Labelled as generated everywhere.
        const std::filesystem::path trace_dir = std::filesystem::temp_directory_path() / "sessionscope_demo" / "demo";
        write_simulated_trace(trace_dir, "demo");   // a short song that chokes within seconds
        note = "Demo: generated data, not a measurement.";
        auto sensor = std::make_unique<ReplaySensor>(trace_dir);
        auto host = std::make_unique<ReplayHost>(*static_cast<ReplaySensor *>(sensor.get()));
        return std::make_unique<HudEngine>(std::move(sensor), std::move(host), "demo", note);
    }

    // Live
    auto sensor = std::make_unique<LiveSensor>();
    auto host = std::make_unique<LiveHost>();
    return std::make_unique<HudEngine>(std::move(sensor), std::move(host), "live", std::nullopt);
}

static void show_error(const std::string &msg) {
#ifdef _WIN32
    MessageBoxA(nullptr, msg.c_str(), "SessionScope HUD Error", MB_OK | MB_ICONERROR);
#else
    std::cerr << msg << "\n";
#endif
}

// Whether the sensor plug-in is in a standard VST3 or CLAP folder, for the waiting checklist.
static bool sensor_installed() {
#ifdef _WIN32
    namespace fs = std::filesystem;
    // <folder named by the environment variable>/<below>, or empty if the variable is not set.
    const auto file_in = [](const char *variable, const fs::path &below) -> fs::path {
        char base[MAX_PATH] = {};
        if (!GetEnvironmentVariableA(variable, base, MAX_PATH)) return {};
        return fs::path(base) / below;
    };
    const fs::path vst3_file = fs::path("SessionScope.vst3") / "Contents" / "x86_64-win" / "SessionScope.vst3";
    const fs::path candidates[] = {
        file_in("LOCALAPPDATA", fs::path("Programs/Common/VST3") / vst3_file),
        file_in("CommonProgramFiles", fs::path("VST3") / vst3_file),
        file_in("LOCALAPPDATA", "Programs/Common/CLAP/SessionScope.clap"),
        file_in("CommonProgramFiles", "CLAP/SessionScope.clap"),
    };
    std::error_code error;
    for (const fs::path &file : candidates) {
        if (!file.empty() && fs::exists(file, error)) return true;
    }
    return false;
#elif defined(__APPLE__)
    // Every format has a per-user folder under ~/Library and a shared one under /Library.
    namespace fs = std::filesystem;
    const char *home = std::getenv("HOME");
    const fs::path plugins_below = "Library/Audio/Plug-Ins";
    const fs::path bundles[] = {"VST3/SessionScope.vst3", "CLAP/SessionScope.clap", "Components/SessionScope.component"};
    std::error_code error;
    for (const fs::path &bundle : bundles) {
        if (home && fs::exists(fs::path(home) / plugins_below / bundle, error)) return true;
        if (fs::exists(fs::path("/") / plugins_below / bundle, error)) return true;
    }
    return false;
#else
    return false;
#endif
}

static int run_hud(const Options &opt) {
#ifdef _WIN32
    // Sleeps wake within 1 ms instead of Windows' default 15.6 ms, so pacing frames never misses one.
    timeBeginPeriod(1);
#endif
    // What the HUD remembers between runs (hud.ini). A screenshot run neither reads nor writes it,
    // so its picture never depends on how someone left the window.
    const bool remember = !opt.screenshot;
    const std::filesystem::path settings_file = remember ? hud_settings_file() : std::filesystem::path();
    HudSettings settings = remember ? load_hud_settings(settings_file) : HudSettings{};

    std::string error;
    HudWindowOptions win_opts;
    win_opts.always_on_top = settings.on_top;
    win_opts.place = settings.compact_mode ? settings.compact : settings.full;
    if (settings.compact_mode) win_opts.min_width = win_opts.min_height = 80;   // set properly below
    auto window = HudWindow::create(win_opts, error);
    if (!window) {
        show_error(error);
        return 1;
    }

    std::string demo_note;
    auto engine = make_engine(opt, demo_note);
    // Windows sizes windows in pixels, so the layout grows with the display scale. macOS sizes
    // them in points, already scaled, so there only the text is rasterised finer.
    const double pixels_per_unit = window->framebuffer_scale();
    const double layout_scale = window->content_scale() / pixels_per_unit;
    HudView view(window->textures(), layout_scale, pixels_per_unit);   // fonts before the first frame
    view.apply(settings);

    // Sizes in the web HUD's CSS pixels, turned into window units. The full layout keeps the
    // window's original minimum; compact mode is a strip: the waveform and the readout.
    const auto units = [layout_scale](double css) { return static_cast<int>(std::lround(css * layout_scale)); };
    const WindowPlace compact_default{0, 0, units(640), units(150)};
    const int compact_min_width = units(520), compact_min_height = units(110);
    if (view.compact()) window->set_min_size(compact_min_width, compact_min_height);

    // Switches the window between its two sizes, each remembered separately.
    const auto switch_window_size = [&](bool to_compact) {
        const WindowPlace now = window->place();
        if (to_compact) {
            settings.full = now;
            WindowPlace target = settings.compact.value_or(compact_default);
            if (!settings.compact) {   // the first time: where the window is now
                target.x = now.x;
                target.y = now.y;
            }
            window->set_min_size(compact_min_width, compact_min_height);
            window->set_place(target);
        } else {
            settings.compact = now;
            WindowPlace target = settings.full.value_or(WindowPlace{now.x, now.y, units(960), units(620)});
            window->set_min_size(win_opts.min_width, win_opts.min_height);
            window->set_place(target);
        }
    };

    // The engine runs ten times a second; the window draws every display frame (VSync).
    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now();
    constexpr auto tick_interval = std::chrono::milliseconds(100);
    const auto screenshot_time = clock::now() + std::chrono::milliseconds(opt.wait_ms);
    bool screenshot_taken = false;
    bool installed = sensor_installed();
    const double frame_period = window->frame_period();
    constexpr double SWAP_MARGIN_S = 0.004;      // wake this long before the display wants the next frame
    constexpr double MIN_PACED_PERIOD_S = 0.007; // above about 140 Hz a frame is too short to sleep in
    auto last_swap = clock::now();
    auto installed_checked_at = clock::now();

    while (!window->should_close()) {
        // Stopped or waiting: nothing slides, so draw again only for the next engine update or
        // the mouse (at most 100 ms away), instead of on every display frame.
        if (!view.animating() && !opt.screenshot) {
            window->wait_for_input(std::chrono::duration<double>(next_tick - clock::now()).count());
        } else if (!window->paces_frames() && frame_period > MIN_PACED_PERIOD_S) {
            // Playing, on OpenGL: sleep through most of the frame here. Some graphics drivers
            // otherwise spin a core inside the buffer swap while they wait for the display
            // (measured: 15 % of a core without this, 6 % with it). On Windows the renderer waits
            // on the swap chain's frame-latency object instead, which needs no guessing.
            const double until_swap = std::chrono::duration<double>(last_swap - clock::now()).count() + frame_period;
            window->wait_for_input(until_swap - SWAP_MARGIN_S);
        }

        const auto now = clock::now();
        if (now >= next_tick) {
            engine->select_bar(view.selected_bar());
            engine->tick();
            view.receive(engine->snapshot(std::chrono::duration<double>(now.time_since_epoch()).count()));
            next_tick += tick_interval;
            if (next_tick < now) next_tick = now;   // fell behind: catch up rather than burst
        }
        if (now - installed_checked_at > std::chrono::seconds(3)) {   // a file check: not every frame
            installed = sensor_installed();
            installed_checked_at = now;
        }

        window->begin_frame();
        const ViewRequests requests = view.draw(installed);
        if (requests.keep_on_top) window->set_on_top(*requests.keep_on_top);
        if (requests.compact) switch_window_size(*requests.compact);

        // The frame is read while it is drawn, so the request has to come before end_frame.
        const bool take_screenshot = opt.screenshot && !screenshot_taken && now >= screenshot_time;
        if (take_screenshot) window->request_screenshot(*opt.screenshot);

        window->end_frame();   // waits for the display (VSync) while playing, so the loop never spins a core
        last_swap = clock::now();

        if (take_screenshot) {
            screenshot_taken = true;
            if (!window->screenshot_saved()) {
                show_error("Could not save the screenshot to " + opt.screenshot->u8string());
                return 1;
            }
            break;   // a screenshot run quits once it has its frame
        }
    }

    if (remember) {
        (view.compact() ? settings.compact : settings.full) = window->place();
        view.remember(settings);
        save_hud_settings(settings_file, settings);
    }
    return 0;
}

}  // namespace sessionscope

// -----------------------------------------------------------------------------
// Entry point: main on console, WinMain on Windows subsystem

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Convert command line to argc/argv
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return 1;

    std::vector<std::string> argv_utf8;
    argv_utf8.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        argv_utf8.push_back(s);
    }
    LocalFree(wargv);

    std::vector<char *> argv_ptrs;
    argv_ptrs.reserve(argc);
    for (auto &s : argv_utf8) argv_ptrs.push_back(s.data());

    return sessionscope::run_hud(sessionscope::parse_args(argc, argv_ptrs.data()));
}
#else
int main(int argc, char **argv) {
    return sessionscope::run_hud(sessionscope::parse_args(argc, argv));
}
#endif
