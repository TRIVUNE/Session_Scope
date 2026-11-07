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

// stb hands the finished PNG over in pieces; this writes each piece to the open file.
static void append_to_file(void *file, void *data, int size) {
    static_cast<std::ofstream *>(file)->write(static_cast<const char *>(data), size);
}

std::unique_ptr<HudWindow> HudWindow::create(const HudWindowOptions &opts, std::string &error) {
    if (!glfwInit()) {
        error = "Failed to initialize GLFW";
        return nullptr;
    }

    // "Hints" are settings for the next window GLFW makes.
    Renderer::set_window_hints();                       // what the renderer needs (OpenGL or not)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);           // hidden until the first frame is ready
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);   // grow with Windows' display scale (125 %, 150 %...)
    glfwWindowHint(GLFW_FLOATING, opts.always_on_top ? GLFW_TRUE : GLFW_FALSE);   // above other windows

    GLFWwindow *window = glfwCreateWindow(opts.width, opts.height, opts.title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
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

    // Dear ImGui keeps its state in a "context"; make it before the renderer, which sets up
    // ImGui's drawing code.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // ImGui would otherwise write its own imgui.ini; we have hud.ini

    hud->renderer_ = Renderer::create(window, error);
    if (!hud->renderer_) return nullptr;   // `hud` is destroyed here, which closes the window

    // Show one frame in the background colour, then the window: it never flashes black or white.
    hud->renderer_->present_blank();
    glfwShowWindow(window);
    return hud;
}

HudWindow::HudWindow(GLFWwindow *window) : window_(window) {}

HudWindow::~HudWindow() {
    renderer_.reset();   // shut ImGui's drawing code down while the window still exists
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

bool HudWindow::should_close() const { return glfwWindowShouldClose(window_); }

bool HudWindow::is_minimized() const { return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE; }

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
