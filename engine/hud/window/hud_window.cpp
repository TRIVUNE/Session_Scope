// =================================================================================================
// hud_window.cpp - the HUD's window (see hud_window.h)
// =================================================================================================

#include "window/hud_window.h"

#define GLFW_INCLUDE_NONE              // GLFW must not include any OpenGL header itself
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

// stb_image_write: a one-file PNG writer (public domain, github.com/nothings/stb, fetched pinned by
// tools/get_hud_deps.py). Defining STB_IMAGE_WRITE_IMPLEMENTATION in exactly one .cpp file turns
// its header into code. STBI_WRITE_NO_STDIO: we open the file ourselves (C++ streams handle
// paths with any characters, its own C file code doesn't on Windows).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace sessionscope {

namespace {

// stb hands the finished PNG over in pieces; this writes each piece to the open file.
void append_to_file(void *file, void *data, int size) {
    static_cast<std::ofstream *>(file)->write(static_cast<const char *>(data), size);
}

// How many HUD windows exist. GLFW is started with the first and shut down after the last, so
// closing a pop-out window doesn't take GLFW away from the others.
int g_windows = 0;

// ---- Mouse and keyboard, routed to the right window's Dear ImGui.
// Each window has its own ImGui "context" (its own state). glfwPollEvents() delivers the events of
// every window at once, so each event must first switch to the context of the window it belongs
// to, then hand the event to Dear ImGui's GLFW code. (Dear ImGui's own advice for several windows
// with several contexts: the comment above ImGui_ImplGlfw_GetBackendData in imgui_impl_glfw.cpp.)
class SwitchTo {   // switches ImGui to a window's context, and back again when it goes out of scope
public:
    explicit SwitchTo(GLFWwindow *window) : previous_(ImGui::GetCurrentContext()) {
        auto *hud = static_cast<HudWindow *>(glfwGetWindowUserPointer(window));
        if (hud) ImGui::SetCurrentContext(hud->imgui_context());
    }
    ~SwitchTo() { ImGui::SetCurrentContext(previous_); }

private:
    ImGuiContext *previous_;
};

void on_focus(GLFWwindow *w, int focused) { SwitchTo s(w); ImGui_ImplGlfw_WindowFocusCallback(w, focused); }
void on_enter(GLFWwindow *w, int entered) { SwitchTo s(w); ImGui_ImplGlfw_CursorEnterCallback(w, entered); }
void on_move(GLFWwindow *w, double x, double y) { SwitchTo s(w); ImGui_ImplGlfw_CursorPosCallback(w, x, y); }
void on_button(GLFWwindow *w, int button, int action, int mods) { SwitchTo s(w); ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods); }
void on_scroll(GLFWwindow *w, double dx, double dy) { SwitchTo s(w); ImGui_ImplGlfw_ScrollCallback(w, dx, dy); }
void on_key(GLFWwindow *w, int key, int scancode, int action, int mods) { SwitchTo s(w); ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods); }
void on_char(GLFWwindow *w, unsigned int c) { SwitchTo s(w); ImGui_ImplGlfw_CharCallback(w, c); }

}  // namespace

std::unique_ptr<HudWindow> HudWindow::create(const HudWindowOptions &opts, std::string &error) {
    if (g_windows == 0 && !glfwInit()) {
        error = "Failed to initialize GLFW";
        return nullptr;
    }
    ++g_windows;   // counted from here; ~HudWindow, or the failure paths below, count it back down

    // "Hints" are settings for the next window GLFW makes.
    Renderer::set_window_hints();                       // what the renderer needs (OpenGL or not)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);           // hidden until the first frame is ready
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);   // grow with Windows' display scale (125 %, 150 %...)
    glfwWindowHint(GLFW_FLOATING, opts.always_on_top ? GLFW_TRUE : GLFW_FALSE);   // above other windows

    GLFWwindow *window = glfwCreateWindow(opts.width, opts.height, opts.title.c_str(), nullptr, nullptr);
    if (!window) {
        if (--g_windows == 0) glfwTerminate();
        error = "Failed to create the HUD window.";
        return nullptr;
    }

    glfwSetWindowSizeLimits(window, opts.min_width, opts.min_height, GLFW_DONT_CARE, GLFW_DONT_CARE);
    // Back where it was last time, if that is still on a screen. Done before it's shown, so the
    // window doesn't visibly jump.
    if (opts.place && HudWindow::is_on_a_screen(*opts.place)) {
        glfwSetWindowSize(window, opts.place->width, opts.place->height);
        glfwSetWindowPos(window, opts.place->x, opts.place->y);
    }

    auto hud = std::unique_ptr<HudWindow>(new HudWindow(window));

    // Dear ImGui keeps its state in a "context"; each window has its own. Made before the renderer,
    // which sets up ImGui's drawing code, and left current so the caller can load fonts into it.
    IMGUI_CHECKVERSION();
    hud->context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(hud->context_);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // ImGui would otherwise write its own imgui.ini; we have hud.ini

    hud->renderer_ = Renderer::create(window, error);
    if (!hud->renderer_) return nullptr;   // `hud` is destroyed here, which closes the window

    // Mouse and keyboard: our routing functions above, which find this HudWindow through GLFW's
    // "user pointer".
    glfwSetWindowUserPointer(window, hud.get());
    glfwSetWindowFocusCallback(window, on_focus);
    glfwSetCursorEnterCallback(window, on_enter);
    glfwSetCursorPosCallback(window, on_move);
    glfwSetMouseButtonCallback(window, on_button);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
    glfwSetCharCallback(window, on_char);

    // Show one frame in the background colour, then the window: it never flashes black or white.
    hud->renderer_->present_blank();
    glfwShowWindow(window);
    return hud;
}

HudWindow::HudWindow(GLFWwindow *window) : window_(window) {}

HudWindow::~HudWindow() {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    if (context_) ImGui::SetCurrentContext(context_);   // shut down THIS window's ImGui, not another's
    renderer_.reset();   // shut ImGui's drawing code down while the window still exists
    if (context_) ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous == context_ ? nullptr : previous);
    if (window_) {
        glfwSetWindowUserPointer(window_, nullptr);
        glfwDestroyWindow(window_);
    }
    if (--g_windows == 0) glfwTerminate();
}

bool HudWindow::should_close() const { return glfwWindowShouldClose(window_); }

bool HudWindow::is_minimized() const { return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE; }

void HudWindow::set_visible(bool visible) {
    if (visible) glfwShowWindow(window_);
    else glfwHideWindow(window_);
}

bool HudWindow::is_visible() const { return glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_TRUE; }

void HudWindow::set_title(const std::string &title) { glfwSetWindowTitle(window_, title.c_str()); }

void HudWindow::set_on_top(bool on) { glfwSetWindowAttrib(window_, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE); }

void HudWindow::wait_for_input(double seconds) {
    if (seconds > 0) glfwWaitEventsTimeout(seconds);
}

int HudWindow::refresh_rate() const {
    // The window's middle point...
    int x, y, w, h;
    glfwGetWindowPos(window_, &x, &y);
    glfwGetWindowSize(window_, &w, &h);
    const int mid_x = x + w / 2, mid_y = y + h / 2;
    // ...and the screen it's on (GLFW calls screens "monitors"). Off every screen: the main one.
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    GLFWmonitor *found = glfwGetPrimaryMonitor();
    for (int m = 0; m < count; ++m) {
        int mx, my;
        glfwGetMonitorPos(monitors[m], &mx, &my);
        const GLFWvidmode *mode = glfwGetVideoMode(monitors[m]);
        if (mode && mid_x >= mx && mid_x < mx + mode->width && mid_y >= my && mid_y < my + mode->height) found = monitors[m];
    }
    const GLFWvidmode *mode = found ? glfwGetVideoMode(found) : nullptr;
    return mode && mode->refreshRate > 0 ? mode->refreshRate : 60;
}

double HudWindow::frame_period() const { return static_cast<double>(refreshes_per_frame_) / refresh_rate(); }

void HudWindow::set_frame_limit(int fps) {
    fps_limit_ = fps;
    update_frame_limit();
}

void HudWindow::update_frame_limit() {
    // How many refreshes per frame come closest to the limit: 120 Hz with a 60 limit -> 2,
    // 60 Hz with a 60 limit -> 1, 144 Hz with 30 -> 5 (28.8 frames a second).
    const int refresh = refresh_rate();
    int refreshes = 1;
    if (fps_limit_ > 0) refreshes = std::max(1, static_cast<int>(std::lround(static_cast<double>(refresh) / fps_limit_)));
    if (refreshes != refreshes_per_frame_) {
        refreshes_per_frame_ = refreshes;
        renderer_->set_frames_per_refresh(refreshes);
    }
}

// GLFW makes the window "layered" and tells Windows its opacity; Windows blends it with whatever
// is behind. (macOS: the same through Cocoa.)
void HudWindow::set_opacity(int percent) {
    glfwSetWindowOpacity(window_, static_cast<float>(std::clamp(percent, 25, 100)) / 100.0f);
}

void HudWindow::begin_frame(const std::function<void()> &before_new_frame) {
    ImGui::SetCurrentContext(context_);   // this window's ImGui, until the next window's begin_frame
    renderer_->begin_frame();   // on Windows: sleeps here until the screen can take a new frame
    glfwPollEvents();           // after the wait, so this frame sees the newest mouse movement
    ImGui_ImplGlfw_NewFrame();  // tell ImGui where the mouse is, the window size, ...
    if (before_new_frame) before_new_frame();
    ImGui::NewFrame();

    // ImGui draws inside "ImGui windows". We make one that covers our whole real window, with no
    // title, border or background (the view paints its own background), and draw everything in it.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // see-through
    ImGui::Begin("##bg", nullptr,   // "##" = a name ImGui uses internally but never shows
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav);
}

void HudWindow::end_frame() {
    // Close what begin_frame() opened, in reverse order.
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::Render();       // ImGui turns this frame's boxes and text into triangles
    renderer_->render();   // the graphics card draws them

    // A screenshot must read the frame before it is shown: afterwards the picture is gone.
    if (!screenshot_path_.empty()) {
        screenshot_saved_ = save_screenshot(screenshot_path_);
        screenshot_path_.clear();
    }
    renderer_->present();  // on screen at the next refresh
}

void HudWindow::request_screenshot(const std::filesystem::path &path) { screenshot_path_ = path; }

WindowPlace HudWindow::place() const {
    WindowPlace p;
    glfwGetWindowPos(window_, &p.x, &p.y);
    glfwGetWindowSize(window_, &p.width, &p.height);
    return p;
}

void HudWindow::set_place(const WindowPlace &place) {
    glfwSetWindowSize(window_, place.width, place.height);
    glfwSetWindowPos(window_, place.x, place.y);
}

void HudWindow::set_min_size(int width, int height) {
    glfwSetWindowSizeLimits(window_, width, height, GLFW_DONT_CARE, GLFW_DONT_CARE);
}

bool HudWindow::is_on_a_screen(const WindowPlace &place) {
    int count = 0;
    GLFWmonitor **monitors = glfwGetMonitors(&count);
    const int corners[4][2] = {{place.x, place.y},
                               {place.x + place.width, place.y},
                               {place.x, place.y + place.height},
                               {place.x + place.width, place.y + place.height}};
    for (int m = 0; m < count; ++m) {
        int x, y, w, h;   // the screen's usable area (without the taskbar)
        glfwGetMonitorWorkarea(monitors[m], &x, &y, &w, &h);
        for (const auto &corner : corners) {
            if (corner[0] >= x && corner[0] < x + w && corner[1] >= y && corner[1] < y + h) return true;
        }
    }
    return false;
}

double HudWindow::content_scale() const {
    float xscale, yscale;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    return (xscale + yscale) * 0.5;
}

double HudWindow::framebuffer_scale() const {
    int window_w, window_h, framebuffer_w, framebuffer_h;
    glfwGetWindowSize(window_, &window_w, &window_h);
    glfwGetFramebufferSize(window_, &framebuffer_w, &framebuffer_h);
    return window_w > 0 ? static_cast<double>(framebuffer_w) / window_w : 1.0;
}

bool HudWindow::save_screenshot(const std::filesystem::path &path) const {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;   // red, green, blue per pixel: the window is never see-through
    if (!renderer_->read_pixels(width, height, pixels)) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const bool written = stbi_write_png_to_func(append_to_file, &file, width, height, 3, pixels.data(), width * 3) != 0;
    return written && static_cast<bool>(file);
}

}  // namespace sessionscope
