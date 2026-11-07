// SessionScope HUD native entry point.
// Builds build/engine/SessionScopeHUD.exe
// Options: --replay TRACE | --demo [--vertical] [--screenshot OUT.png] [--wait MS]
// Draws the HUD (hud_view.h) from engine snapshots and the machine's readings (system_monitor.h).

#include "hud_window.h"
#include "hud_engine.h"
#include "hud_settings.h"
#include "hud_view.h"
#include "simulate.h"
#include "system_monitor.h"

#include <algorithm>
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
    bool vertical = false;   // modules stacked top to bottom (for screenshots; normally from hud.ini)
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
        } else if (arg == "--vertical") {
            opt.vertical = true;
        } else {
            // A windowed program has no console, so say it where it can be seen.
            show_error("Unknown or incomplete argument: " + arg +
                       "\n\nUsage: SessionScopeHUD [--replay TRACE] [--demo] [--vertical] [--screenshot OUT.png] [--wait MS]");
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

// The window's size when nothing is saved, and its smallest, in CSS pixels: a long strip, across
// or down.
struct StripSize {
    int width, height, min_width, min_height;
};
constexpr StripSize ACROSS = {1400, 150, 300, 70};
constexpr StripSize DOWN = {280, 1100, 120, 240};

// The demo has no DAW: its stand-in CPU follows the simulated song's lateness, so the meter
// peaks where the song chokes. Labelled as generated, like the rest of the demo.
static double demo_daw_cpu(const Snapshot &snapshot, double now) {
    const double late = snapshot.late.p99 ? std::clamp(*snapshot.late.p99, 0.0, 1.0) : 0.0;
    return std::clamp(0.07 + 0.03 * std::sin(now * 0.9) + 0.6 * late, 0.0, 1.0);
}

static int run_hud(const Options &opt) {
#ifdef _WIN32
    // Sleeps wake within 1 ms instead of Windows' default 15.6 ms.
    timeBeginPeriod(1);
#endif
    // What the HUD remembers between runs (hud.ini). A screenshot run neither reads nor writes it,
    // so its picture never depends on how someone left the window.
    const bool remember = !opt.screenshot;
    const std::filesystem::path settings_file = remember ? hud_settings_file() : std::filesystem::path();
    HudSettings settings = remember ? load_hud_settings(settings_file) : HudSettings{};
    if (opt.vertical) settings.vertical = true;

    std::string error;
    HudWindowOptions win_opts;
    win_opts.always_on_top = settings.on_top;
    win_opts.place = settings.window;
    win_opts.min_width = win_opts.min_height = 60;   // set properly once the display scale is known
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

    const auto units = [layout_scale](int css) { return static_cast<int>(std::lround(css * layout_scale)); };
    const auto strip = [](bool vertical) { return vertical ? DOWN : ACROSS; };
    window->set_min_size(units(strip(view.vertical()).min_width), units(strip(view.vertical()).min_height));
    if (!settings.window) {   // first run, or a screenshot run: the default strip
        WindowPlace place = window->place();
        place.width = units(strip(view.vertical()).width);
        place.height = units(strip(view.vertical()).height);
        window->set_place(place);
    }

    // Across <-> down: the window turns, keeping its corner.
    const auto turn_window = [&](bool vertical) {
        WindowPlace place = window->place();
        std::swap(place.width, place.height);
        const StripSize size = strip(vertical);
        window->set_min_size(units(size.min_width), units(size.min_height));
        place.width = std::max(place.width, units(size.min_width));
        place.height = std::max(place.height, units(size.min_height));
        window->set_place(place);
    };

    // The engine and the CPU readings run ten times a second; the window draws every display
    // frame, so the heatmap slides smoothly in between.
    using clock = std::chrono::steady_clock;
    const auto seconds = [](clock::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); };
    auto next_tick = clock::now();
    constexpr auto tick_interval = std::chrono::milliseconds(100);
    const auto screenshot_time = clock::now() + std::chrono::milliseconds(opt.wait_ms);
    bool screenshot_taken = false;
    const double frame_period = window->frame_period();
    constexpr double SWAP_MARGIN_S = 0.004;      // wake this long before the display wants the next frame
    constexpr double MIN_PACED_PERIOD_S = 0.007; // above about 140 Hz a frame is too short to sleep in
    auto last_swap = clock::now();

    SystemMonitor system;
    Snapshot snapshot;
    const bool live = opt.replay_path.empty() && !opt.demo;

    while (!window->should_close()) {
        if (!window->paces_frames() && frame_period > MIN_PACED_PERIOD_S) {
            // OpenGL: sleep through most of the frame here. Some graphics drivers otherwise spin a
            // core inside the buffer swap while they wait for the display (measured: 15 % of a
            // core without this, 6 % with it). On Windows the renderer waits on the swap chain's
            // frame-latency object instead, which needs no guessing.
            const double until_swap = std::chrono::duration<double>(last_swap - clock::now()).count() + frame_period;
            window->wait_for_input(until_swap - SWAP_MARGIN_S);
        }

        const auto now = clock::now();
        if (now >= next_tick) {
            engine->tick();
            snapshot = engine->snapshot(seconds(now));
            const std::optional<int> bar = snapshot.playing ? snapshot.bar : std::nullopt;
            // A replayed trace's process is long gone (its id may now be another program's).
            const double host = live ? engine->source().host_process_id() : 0;
            const std::optional<double> stand_in =
                opt.demo ? std::optional<double>(demo_daw_cpu(snapshot, seconds(now))) : std::nullopt;
            system.tick(seconds(now), host, bar, stand_in);
            next_tick += tick_interval;
            if (next_tick < now) next_tick = now;   // fell behind: catch up rather than burst
        }

        window->begin_frame();   // on Windows: waits here until the display can take the frame
        const ViewRequests requests = view.draw(system, snapshot, seconds(clock::now()));
        if (requests.keep_on_top) window->set_on_top(*requests.keep_on_top);
        if (requests.vertical) turn_window(*requests.vertical);

        // The frame is read while it is drawn, so the request has to come before end_frame.
        const bool take_screenshot = opt.screenshot && !screenshot_taken && now >= screenshot_time;
        if (take_screenshot) window->request_screenshot(*opt.screenshot);

        window->end_frame();
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
        settings.window = window->place();
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
