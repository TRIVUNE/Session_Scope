// =================================================================================================
// hud_app.cpp - runs the HUD from start to finish (see hud_app.h)
// =================================================================================================

#include "hud_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "hud_engine.h"        // HudEngine: turns sensor data into snapshots
#include "hud_settings.h"      // hud.ini
#include "demo_probes.h"       // the demo's made-up probes
#include "hud_view.h"          // the modules and the menu
#include "probe_monitor.h"     // CPU per track and plug-in
#include "simulate.h"          // the demo's generated song
#include "system_monitor.h"    // readings from this computer
#include "window/desktop.h"
#include "window/hud_window.h"

#include <imgui.h>   // --test-drag, and making each window's ImGui current

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod
#endif

namespace sessionscope {

namespace {

// The window's size the first time (nothing saved yet), and its smallest, in design pixels: a long
// strip, across or down. MiniMeters' defaults are similar: 1920 x 128 across, 200 x 1080 down.
struct StripSize {
    int width, height, min_width, min_height;
};
constexpr StripSize ACROSS = {1400, 150, 300, 70};
constexpr StripSize DOWN = {280, 1100, 120, 240};

// How often the engine and the SystemMonitor read new data.
constexpr auto TICK = std::chrono::milliseconds(100);

// Makes the engine for the chosen data source.
std::unique_ptr<HudEngine> make_engine(const HudOptions &options) {
    if (!options.replay_path.empty()) {   // a recording made earlier (a trace folder)
        auto sensor = std::make_unique<ReplaySensor>(options.replay_path);
        auto host = std::make_unique<ReplayHost>(*sensor);
        return std::make_unique<HudEngine>(std::move(sensor), std::move(host), "replay", std::nullopt);
    }
    if (options.demo) {   // a generated song, played back: the HUD without any DAW
        const auto folder = std::filesystem::temp_directory_path() / "sessionscope_demo" / "demo";
        write_simulated_trace(folder, "demo");   // a short song that chokes within seconds
        auto sensor = std::make_unique<ReplaySensor>(folder);
        auto host = std::make_unique<ReplayHost>(*sensor);
        return std::make_unique<HudEngine>(std::move(sensor), std::move(host), "demo",
                                           std::string("Demo: generated data, not a measurement."));
    }
    // Live: the sensor plug-in in a DAW, through shared memory.
    return std::make_unique<HudEngine>(std::make_unique<LiveSensor>(), std::make_unique<LiveHost>(), "live", std::nullopt);
}

// The demo has no DAW, so it has no DAW CPU. This made-up value follows the generated song's
// lateness, so the meters move where the song chokes. Clearly a stand-in, like the rest of the demo.
double demo_daw_cpu(const Snapshot &snapshot, double now) {
    const double late = snapshot.late.p99 ? std::clamp(*snapshot.late.p99, 0.0, 1.0) : 0.0;
    return std::clamp(0.07 + 0.03 * std::sin(now * 0.9) + 0.6 * late, 0.0, 1.0);
}

// --test-drag (testing only): plays a left-button drag into Dear ImGui, one step per frame, as if
// the mouse did it. ImGui handles at most one button change per frame, so the steps are spread out:
//   step 0      the mouse arrives at the start point
//   step 1      the button goes down
//   steps 2-21  the mouse moves to the end point in 20 even steps
//   step 22     the button comes up
void play_test_drag(const std::array<float, 4> &drag, bool started, int &step) {
    if (!started || step > 22) return;
    ImGuiIO &io = ImGui::GetIO();
    const float x1 = drag[0], y1 = drag[1], x2 = drag[2], y2 = drag[3];
    // Every step first states where the mouse is: the real cursor may also be over the window, and
    // its position can arrive in the same frame; ours, added last, is the one that counts.
    const float t = std::clamp(static_cast<float>(step - 1) / 20.0f, 0.0f, 1.0f);
    io.AddMousePosEvent(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t);
    if (step == 1) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    if (step == 22) io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ++step;
}

// A module in its own window (right-click a module > "Pop out"): the window, and a view that shows
// just that module. Closing the window puts the module back at the end of the strip.
struct PopOutWindow {
    std::string module;
    std::unique_ptr<HudWindow> window;
    std::unique_ptr<HudView> view;
};

// Makes the view for a window: works out the window's scale, then loads fonts and modules into the
// window's own Dear ImGui context.
std::unique_ptr<HudView> make_view(HudWindow &window, std::optional<std::string> only_module) {
    ImGui::SetCurrentContext(window.imgui_context());
    // Windows sizes windows in pixels, so the layout must grow with the screen's scale (150 % etc.).
    // macOS sizes them in points, already scaled, so there only the letters are drawn finer.
    const double pixels_per_unit = window.framebuffer_scale();
    const double layout_scale = window.content_scale() / pixels_per_unit;
    return std::make_unique<HudView>(window.textures(), layout_scale, pixels_per_unit, window.glfw(), std::move(only_module));
}

}  // namespace

void show_error(const std::string &message) {
#ifdef _WIN32
    MessageBoxA(nullptr, message.c_str(), "SessionScope HUD Error", MB_OK | MB_ICONERROR);
#else
    std::cerr << message << "\n";
#endif
}

int run_hud(const HudOptions &options) {
#ifdef _WIN32
    // Windows normally wakes sleeping programs in steps of 15.6 ms; this makes it 1 ms, so our
    // sleeps end on time and no frame is missed.
    timeBeginPeriod(1);
#endif

    // ---- 1. settings and the main window
    // A screenshot run neither reads nor writes hud.ini, so its picture never depends on how
    // someone last left the window. It also leaves the tray, hotkey and pop-outs alone.
    const bool remember = !options.screenshot;
    const std::filesystem::path settings_file =
        !remember ? std::filesystem::path() : options.settings_file ? *options.settings_file : hud_settings_file();
    HudSettings settings = remember ? load_hud_settings(settings_file) : HudSettings{};
    if (options.vertical) settings.vertical = true;

    HudWindowOptions window_options;
    window_options.always_on_top = settings.on_top;
    window_options.place = settings.window;
    window_options.min_width = window_options.min_height = 60;   // set properly below, once the screen scale is known
    std::string error;
    auto window = HudWindow::create(window_options, error);
    if (!window) {
        show_error(error);
        return 1;
    }

    // ---- 2. engine, computer readings, view, desktop
    auto engine = make_engine(options);
    auto view = make_view(*window, std::nullopt);   // loads the fonts: before the first frame
    view->apply(settings);
    SystemMonitor system;
    Snapshot snapshot;
    // CPU per track and plug-in: the real probes' table, or in the demo, made-up probes' own table.
    std::unique_ptr<DemoProbes> demo_probes = options.demo ? std::make_unique<DemoProbes>() : nullptr;
    ProbeMonitor probes(demo_probes ? demo_probes->table_name().c_str() : probes::TABLE_NAME);
    const bool live = options.replay_path.empty() && !options.demo;

    // Tray icon, hotkey, taskbar button (Windows). You can't have both no taskbar button and no
    // tray icon: then the HUD could get lost behind other windows, so the tray icon wins.
    std::unique_ptr<Desktop> desktop = remember ? std::make_unique<Desktop>() : nullptr;
    if (!settings.taskbar) settings.tray = true;
    if (desktop) {
        desktop->set_tray_icon(settings.tray);
        desktop->set_hotkey(settings.hotkey);
    }
    // Everything a window takes from the settings, for the main window and every pop-out.
    const auto set_up_window = [&](HudWindow &w) {
        w.set_frame_limit(settings.fps);
        if (settings.opacity < 100) w.set_opacity(settings.opacity);
        if (desktop) Desktop::set_in_taskbar(w.glfw(), settings.taskbar);
    };
    set_up_window(*window);

    // Main window size: the default strip on the first run, and the right minimum for the direction.
    const double layout_scale = window->content_scale() / window->framebuffer_scale();
    const auto units = [layout_scale](int design) { return static_cast<int>(std::lround(design * layout_scale)); };
    const auto strip = [](bool vertical) { return vertical ? DOWN : ACROSS; };
    window->set_min_size(units(strip(view->vertical()).min_width), units(strip(view->vertical()).min_height));
    if (!settings.window) {
        WindowPlace place = window->place();
        place.width = units(strip(view->vertical()).width);
        place.height = units(strip(view->vertical()).height);
        window->set_place(place);
    }
    // Switching horizontal <-> vertical turns the window (width and height swap), keeping its corner.
    const auto turn_window = [&](bool vertical) {
        WindowPlace place = window->place();
        std::swap(place.width, place.height);
        const StripSize size = strip(vertical);
        window->set_min_size(units(size.min_width), units(size.min_height));
        place.width = std::max(place.width, units(size.min_width));
        place.height = std::max(place.height, units(size.min_height));
        window->set_place(place);
    };

    // Pop-out windows: the saved ones now, more when the person pops a module out.
    std::vector<PopOutWindow> popouts;
    const auto open_popout = [&](const std::string &module, const WindowPlace &place) {
        HudWindowOptions o;
        o.always_on_top = settings.on_top;
        o.place = place;
        o.width = place.width;
        o.height = place.height;
        o.min_width = o.min_height = units(60);
        o.title = "SessionScope - " + module;
        auto w = HudWindow::create(o, error);
        if (!w) return;   // can't open one more window: the module simply stays out of view
        set_up_window(*w);
        auto v = make_view(*w, module);
        v->apply(settings);
        w->set_title("SessionScope - " + v->first_title());   // "SessionScope - Busiest core"
        popouts.push_back(PopOutWindow{module, std::move(w), std::move(v)});
    };
    if (!options.screenshot) {
        for (const PopOut &p : settings.popouts) open_popout(p.module, p.place);
    }

    // ---- 3. the loop
    using clock = std::chrono::steady_clock;
    const auto seconds = [](clock::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); };
    auto next_tick = clock::now();
    const auto screenshot_time = clock::now() + std::chrono::milliseconds(options.wait_ms);
    bool screenshot_taken = false;
    // For OpenGL (macOS) only: we sleep through most of each frame ourselves, waking this long
    // before the screen wants the next one. Without it some drivers keep a core busy while waiting
    // (measured: 15 % of a core without, 6 % with). Windows waits on the swap chain instead.
    constexpr double WAKE_EARLY_S = 0.004;
    constexpr double SHORTEST_SLEEPABLE_FRAME_S = 0.007;   // above ~140 Hz a frame is too short to sleep in
    auto last_present = clock::now();
    // --test-drag starts halfway to the screenshot, so the screenshot shows the result.
    const auto drag_time = clock::now() + std::chrono::milliseconds(options.wait_ms / 2);
    int drag_step = 0;
    bool hidden = false;   // hidden by the hotkey or the tray icon
    bool quit = false;

    while (!window->should_close() && !quit) {
        // (a) Pacing on OpenGL: sleep until just before the next screen refresh.
        const double frame_period = window->frame_period();
        if (!window->paces_frames() && frame_period > SHORTEST_SLEEPABLE_FRAME_S) {
            const double until_next = std::chrono::duration<double>(last_present - clock::now()).count() + frame_period;
            window->wait_for_input(until_next - WAKE_EARLY_S);
        }

        // (b) Ten times a second: new data.
        const auto now = clock::now();
        if (now >= next_tick) {
            engine->tick();
            snapshot = engine->snapshot(seconds(now));
            const std::optional<int> bar = snapshot.playing ? snapshot.bar : std::nullopt;
            // A replayed recording's DAW is long gone (its process id may now belong to another program).
            const double daw_process = live ? engine->source().host_process_id() : 0;
            const std::optional<double> stand_in =
                options.demo ? std::optional<double>(demo_daw_cpu(snapshot, seconds(now))) : std::nullopt;
            system.tick(seconds(now), daw_process, bar, stand_in);
            if (demo_probes) demo_probes->tick(std::chrono::duration<double>(TICK).count(), snapshot.late.p99.value_or(0) * 4);
            probes.tick(daw_process);
            // A window may have been dragged to a screen with another refresh rate.
            window->update_frame_limit();
            for (PopOutWindow &p : popouts) p.window->update_frame_limit();
            next_tick += TICK;
            if (next_tick < now) next_tick = now;   // fell behind (the computer was busy): don't try to catch up in a burst
        }

        // (c) The tray icon and the hotkey: show / hide everything, or quit.
        if (desktop) {
            const Desktop::Events events = desktop->take_events();
            if (events.quit) quit = true;
            if (events.toggle) {
                hidden = !hidden;
                window->set_visible(!hidden);
                for (PopOutWindow &p : popouts) p.window->set_visible(!hidden);
            }
        }
        // Pop-out windows closed with their X: the module goes back into the strip.
        for (auto it = popouts.begin(); it != popouts.end();) {
            if (it->window->should_close()) {
                view->add_module(it->module);
                it = popouts.erase(it);
            } else {
                ++it;
            }
        }

        // (d) Which windows to draw: the ones someone can see. If none (all hidden or minimised),
        // keep measuring (dropouts still count) and sleep until the next tick or a click.
        const auto can_be_seen = [&](HudWindow &w) { return options.screenshot || (w.is_visible() && !w.is_minimized()); };
        bool any = can_be_seen(*window);
        for (PopOutWindow &p : popouts) any = any || can_be_seen(*p.window);
        if (!any) {
            window->wait_for_input(std::max(0.001, std::chrono::duration<double>(next_tick - clock::now()).count()));
            continue;
        }
        DesktopState desktop_state;
        if (desktop) desktop_state = DesktopState{Desktop::available(), settings.taskbar, settings.tray, desktop->hotkey_label()};

        // (e) The main window's frame.
        bool take_screenshot = false;
        if (can_be_seen(*window)) {
            window->begin_frame([&] {   // on Windows: sleeps here until the screen can take the frame
                if (options.test_drag) play_test_drag(*options.test_drag, now >= drag_time, drag_step);
            });
            view->set_desktop_state(desktop_state);
            const ViewRequests r = view->draw(system, probes, snapshot, seconds(clock::now()));
            if (r.keep_on_top) {
                settings.on_top = *r.keep_on_top;
                window->set_on_top(settings.on_top);
            }
            if (r.vertical) turn_window(*r.vertical);
            if (r.fps) {
                settings.fps = *r.fps;
                window->set_frame_limit(settings.fps);
                for (PopOutWindow &p : popouts) p.window->set_frame_limit(settings.fps);
            }
            if (r.opacity) {
                settings.opacity = *r.opacity;
                window->set_opacity(settings.opacity);
                for (PopOutWindow &p : popouts) p.window->set_opacity(settings.opacity);
            }
            if (desktop && r.taskbar) {
                settings.taskbar = *r.taskbar;
                if (!settings.taskbar && !settings.tray) desktop->set_tray_icon(settings.tray = true);   // never neither
                Desktop::set_in_taskbar(window->glfw(), settings.taskbar);
                for (PopOutWindow &p : popouts) Desktop::set_in_taskbar(p.window->glfw(), settings.taskbar);
            }
            if (desktop && r.tray) {
                settings.tray = *r.tray;
                if (!settings.tray && !settings.taskbar) {   // never neither
                    settings.taskbar = true;
                    Desktop::set_in_taskbar(window->glfw(), true);
                    for (PopOutWindow &p : popouts) Desktop::set_in_taskbar(p.window->glfw(), true);
                }
                desktop->set_tray_icon(settings.tray);
            }
            if (r.quit) quit = true;

            take_screenshot = options.screenshot && !screenshot_taken && now >= screenshot_time;
            if (take_screenshot) window->request_screenshot(*options.screenshot);   // must be asked before end_frame()
            window->end_frame();
            last_present = clock::now();

            // A pop-out was asked for: a new window just below the strip, the module's size.
            if (r.pop_out && !options.screenshot) {
                const WindowPlace main_place = window->place();
                WindowPlace place{main_place.x + units(40), main_place.y + main_place.height + units(40),
                                  std::max(units(120), static_cast<int>(r.pop_out_size.x)),
                                  std::max(units(80), static_cast<int>(r.pop_out_size.y))};
                open_popout(*r.pop_out, place);
            }
        }

        // (f) Each pop-out window's frame.
        for (auto it = popouts.begin(); it != popouts.end();) {
            if (!can_be_seen(*it->window)) {
                ++it;
                continue;
            }
            it->window->begin_frame();
            it->view->set_desktop_state(desktop_state);
            const ViewRequests r = it->view->draw(system, probes, snapshot, seconds(clock::now()));
            if (r.keep_on_top) it->window->set_on_top(*r.keep_on_top);
            it->window->end_frame();
            if (r.back_in) {   // "Back into the strip"
                view->add_module(it->module);
                it = popouts.erase(it);
            } else {
                ++it;
            }
        }

        if (take_screenshot) {
            screenshot_taken = true;
            if (!window->screenshot_saved()) {
                show_error("Could not save the screenshot to " + options.screenshot->u8string());
                return 1;
            }
            break;   // a screenshot run ends once it has its picture
        }
    }

    // ---- 4. save the settings
    if (remember) {
        settings.window = window->place();
        view->remember(settings);
        settings.popouts.clear();
        for (const PopOutWindow &p : popouts) settings.popouts.push_back(PopOut{p.module, p.window->place()});
        save_hud_settings(settings_file, settings);
    }
    popouts.clear();   // close the pop-out windows before the main one
    return 0;
}

}  // namespace sessionscope
