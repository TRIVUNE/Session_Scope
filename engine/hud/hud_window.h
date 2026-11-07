// HUD window: GLFW + OpenGL 3.3 core context + ImGui backends.
// Creates an always-on-top window with a dark background from the first frame, on Windows and macOS.

#ifndef SESSIONSCOPE_HUD_WINDOW_H
#define SESSIONSCOPE_HUD_WINDOW_H

#include <memory>
#include <string>
#include <filesystem>
#include <optional>

#include "hud_settings.h"

struct GLFWwindow;

namespace sessionscope {

struct HudWindowOptions {
    int width = 960;
    int height = 620;
    int min_width = 520;
    int min_height = 360;
    std::string title = "SessionScope";
    bool always_on_top = true;
    // Where the window was last time (hud_settings.h). Used only if it is still on a screen.
    std::optional<WindowPlace> place;
};

class HudWindow {
public:
    // Factory: returns null and sets error on failure (e.g. no OpenGL 3.3).
    static std::unique_ptr<HudWindow> create(const HudWindowOptions &opts, std::string &error);

    ~HudWindow();

    // Non-copyable, non-movable (owns GLFWwindow* and ImGui context).
    HudWindow(const HudWindow &) = delete;
    HudWindow &operator=(const HudWindow &) = delete;
    HudWindow(HudWindow &&) = delete;
    HudWindow &operator=(HudWindow &&) = delete;

    bool should_close() const;
    void set_on_top(bool on);
    // Sleeps until the mouse or keyboard does something, or `seconds` pass. For when nothing on
    // screen moves by itself, so the HUD costs next to nothing while the host is stopped.
    void wait_for_input(double seconds);
    // Seconds between display refreshes of the screen the window is on (1/60 if unknown).
    double frame_period() const;
    void begin_frame();
    void end_frame();
    int width() const;
    int height() const;

    // The window's position and size, and moving or resizing it.
    WindowPlace place() const;
    void set_place(const WindowPlace &place);
    void set_min_size(int width, int height);
    // True if at least a corner of the window would be on one of the connected screens, so a
    // window saved on a screen that has since been unplugged doesn't open out of reach.
    static bool is_on_a_screen(const WindowPlace &place);
    // How much larger the display is than a standard 96 dpi one (2 on a 4K laptop or a Retina Mac).
    double content_scale() const;
    // Pixels per window unit: 1 on Windows, where window sizes are already in pixels; 2 on a
    // Retina Mac, where window sizes are in points.
    double framebuffer_scale() const;

    // Saves the next frame as a PNG, read just before it is shown. screenshot_saved() says whether
    // that worked, once end_frame() has run.
    void request_screenshot(const std::filesystem::path &path);
    bool screenshot_saved() const { return screenshot_saved_; }

private:
    explicit HudWindow(GLFWwindow *window);
    bool save_screenshot(const std::filesystem::path &path) const;

    GLFWwindow *window_ = nullptr;
    bool own_context_ = false;  // true if we created the ImGui context
    std::filesystem::path screenshot_path_;
    bool screenshot_saved_ = false;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_WINDOW_H
