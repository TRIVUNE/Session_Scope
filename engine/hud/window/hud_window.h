#ifndef SESSIONSCOPE_HUD_WINDOW_H
#define SESSIONSCOPE_HUD_WINDOW_H

// =================================================================================================
// hud_window.h - the HUD's window
// =================================================================================================
//
// Makes the window, keeps it on top of other windows, remembers where it was, and runs one frame at
// a time:   begin_frame()  ->  (the view draws)  ->  end_frame()
//
// Three pieces work together in here:
//   GLFW        (third-party, github.com/glfw/glfw, zlib licence) - the window, mouse and keyboard
//   Renderer    (window/renderer.h) - puts each frame on screen through the graphics card
//   Dear ImGui  (third-party, github.com/ocornut/imgui, MIT licence) - collects what to draw
//
// Screenshots are written as PNG files by stb_image_write (third-party, public domain).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "hud_settings.h"       // WindowPlace: a window's position and size
#include "window/renderer.h"

struct GLFWwindow;   // defined by GLFW

namespace sessionscope {

// How to make the window.
struct HudWindowOptions {
    int width = 960;                       // size before anything else is decided, in window units
    int height = 620;
    int min_width = 520;                   // the window can't be dragged smaller than this
    int min_height = 360;
    std::string title = "SessionScope";    // shown in the taskbar
    bool always_on_top = true;
    // Where the window was last time (from hud.ini). Used only if it is still on a screen.
    std::optional<WindowPlace> place;
};

class HudWindow {
public:
    // Makes the window. Returns nothing (nullptr) and fills `error` if it can't (no usable
    // graphics card, for example).
    static std::unique_ptr<HudWindow> create(const HudWindowOptions &opts, std::string &error);

    ~HudWindow();

    // A window can't be copied or moved: there is exactly one real window behind it.
    HudWindow(const HudWindow &) = delete;
    HudWindow &operator=(const HudWindow &) = delete;
    HudWindow(HudWindow &&) = delete;
    HudWindow &operator=(HudWindow &&) = delete;

    // True once the person closed the window.
    bool should_close() const;
    // True while the window is minimised to the taskbar: nothing to draw then.
    bool is_minimized() const;
    void set_on_top(bool on);
    // Sleeps until the mouse or keyboard does something, or `seconds` pass.
    void wait_for_input(double seconds);
    // Seconds between the frames the HUD draws: one screen refresh (1/120 s on a 120 Hz screen),
    // or longer when the frame rate is limited.
    double frame_period() const;
    // Limits the frame rate: 0 = every screen refresh, 60 or 30 = about that many frames a second.
    // The HUD can only skip whole refreshes, so on a 144 Hz screen "60" gives every 2nd = 72.
    void set_frame_limit(int fps);
    // Checks which screen the window is on now (it may have been dragged to another one with a
    // different refresh rate) and adjusts the frame limit. Cheap; call about once a second.
    void update_frame_limit();
    // How solid the window is, in percent: 100 = solid, 25 = mostly see-through.
    void set_opacity(int percent);
    // True if begin_frame() itself waits for the screen (Windows), so the caller needn't sleep.
    bool paces_frames() const { return renderer_->paces_frames(); }
    // Where the modules put their pictures (the core heatmap).
    TextureStore &textures() { return *renderer_; }

    // One frame: begin_frame(), then draw with Dear ImGui, then end_frame().
    void begin_frame();
    void end_frame();

    // The window's position and size, and moving or resizing it.
    WindowPlace place() const;
    void set_place(const WindowPlace &place);
    void set_min_size(int width, int height);
    // True if at least one corner of `place` is on a connected screen, so a window saved on a
    // screen that has since been unplugged doesn't open out of reach.
    static bool is_on_a_screen(const WindowPlace &place);
    // How much larger the screen's text is set than a standard 96 dpi screen (1.5 at 150 %).
    double content_scale() const;
    // Screen pixels per window unit: 1 on Windows (sizes are pixels), 2 on a Retina Mac (points).
    double framebuffer_scale() const;

    // Saves the next frame as a PNG file. After end_frame(), screenshot_saved() says if it worked.
    void request_screenshot(const std::filesystem::path &path);
    bool screenshot_saved() const { return screenshot_saved_; }

private:
    explicit HudWindow(GLFWwindow *window);   // use create() instead
    bool save_screenshot(const std::filesystem::path &path) const;
    // The refresh rate (Hz) of the screen under the window's middle; 60 if unknown.
    int refresh_rate() const;

    int fps_limit_ = 0;            // 0 = no limit
    int refreshes_per_frame_ = 1;  // what the limit works out to on the current screen

    GLFWwindow *window_ = nullptr;
    std::unique_ptr<Renderer> renderer_;
    std::filesystem::path screenshot_path_;   // empty: no screenshot wanted
    bool screenshot_saved_ = false;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_WINDOW_H
