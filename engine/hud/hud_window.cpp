// HUD window implementation: GLFW window + Renderer (renderer.h) + ImGui.
// Black from the first frame, always-on-top, in step with the display.
// Screenshots as PNG files, through stb_image_write.

#include "hud_window.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace sessionscope {

// -----------------------------------------------------------------------------
// Screenshots are written by stb_image_write (public domain, .toolchain/stb, fetched by
// tools/get_hud_deps.py): a compressed PNG, about a tenth of the raw pixels.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO   // we open the file ourselves, so paths with any characters work
#include <stb_image_write.h>

// stb hands the PNG over in pieces; this writes them to the open file.
static void append_to_file(void *file, void *data, int size) {
    static_cast<std::ofstream *>(file)->write(static_cast<const char *>(data), size);
}

// -----------------------------------------------------------------------------
// HudWindow implementation

std::unique_ptr<HudWindow> HudWindow::create(const HudWindowOptions &opts, std::string &error) {
    if (!glfwInit()) {
        error = "Failed to initialize GLFW";
        return nullptr;
    }

    Renderer::set_window_hints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // stay hidden until first frame is ready
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_FLOATING, opts.always_on_top ? GLFW_TRUE : GLFW_FALSE);   // above other windows

    GLFWwindow *window = glfwCreateWindow(opts.width, opts.height, opts.title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        error = "Failed to create the HUD window.";
        return nullptr;
    }

    glfwSetWindowSizeLimits(window, opts.min_width, opts.min_height, GLFW_DONT_CARE, GLFW_DONT_CARE);
    // Back where it was last time, if that is still on a screen; before it is shown, so it doesn't jump.
    if (opts.place && HudWindow::is_on_a_screen(*opts.place)) {
        glfwSetWindowSize(window, opts.place->width, opts.place->height);
        glfwSetWindowPos(window, opts.place->x, opts.place->y);
    }

    auto hud = std::unique_ptr<HudWindow>(new HudWindow(window));

    // ImGui context, then the renderer, which sets up ImGui's backends.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // don't write imgui.ini

    hud->renderer_ = Renderer::create(window, error);
    if (!hud->renderer_) return nullptr;   // the destructor closes the window

    // Black from the first frame: show an empty one before the window appears.
    hud->renderer_->present_blank();
    glfwShowWindow(window);
    return hud;
}

HudWindow::HudWindow(GLFWwindow *window) : window_(window) {}

HudWindow::~HudWindow() {
    renderer_.reset();   // shuts down ImGui's backends while the window still exists
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

bool HudWindow::should_close() const {
    return glfwWindowShouldClose(window_);
}

void HudWindow::set_on_top(bool on) {
    glfwSetWindowAttrib(window_, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
}

void HudWindow::wait_for_input(double seconds) {
    if (seconds > 0) glfwWaitEventsTimeout(seconds);
}

double HudWindow::frame_period() const {
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    return mode && mode->refreshRate > 0 ? 1.0 / mode->refreshRate : 1.0 / 60.0;
}

void HudWindow::begin_frame() {
    renderer_->begin_frame();   // on Windows: waits here until the display can take the frame
    glfwPollEvents();           // after the wait, so the frame has the newest input
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Full-screen background: black
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    ImGui::Begin("##bg", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
}

void HudWindow::end_frame() {
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::Render();
    renderer_->render();

    // The frame must be read before it is shown: after the swap the back buffer is undefined.
    if (!screenshot_path_.empty()) {
        screenshot_saved_ = save_screenshot(screenshot_path_);
        screenshot_path_.clear();
    }
    renderer_->present();
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
    const int corners[4][2] = {{place.x, place.y}, {place.x + place.width, place.y},
                               {place.x, place.y + place.height}, {place.x + place.width, place.y + place.height}};
    for (int m = 0; m < count; ++m) {
        int x, y, w, h;
        glfwGetMonitorWorkarea(monitors[m], &x, &y, &w, &h);
        for (const auto &corner : corners) {
            if (corner[0] >= x && corner[0] < x + w && corner[1] >= y && corner[1] < y + h) return true;
        }
    }
    return false;
}

int HudWindow::width() const {
    int w, h;
    glfwGetWindowSize(window_, &w, &h);
    return w;
}

int HudWindow::height() const {
    int w, h;
    glfwGetWindowSize(window_, &w, &h);
    return h;
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
    // RGB is enough: the window is opaque.
    int fb_w = 0, fb_h = 0;
    std::vector<uint8_t> pixels;
    if (!renderer_->read_pixels(fb_w, fb_h, pixels)) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const bool written = stbi_write_png_to_func(append_to_file, &file, fb_w, fb_h, 3, pixels.data(), fb_w * 3) != 0;
    return written && static_cast<bool>(file);
}

}  // namespace sessionscope
