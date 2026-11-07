// =================================================================================================
// renderer_gl.cpp - drawing on macOS (and any non-Windows system): OpenGL 3.3
// =================================================================================================
//
// The macOS version of renderer.h. Simpler than Windows: GLFW makes an OpenGL "context" (OpenGL's
// connection to the graphics card) for the window, and glfwSwapBuffers shows each frame at the next
// screen refresh ("VSync"). macOS has no frame-latency object to wait on, so hud_app.cpp sleeps
// for most of each frame itself (see paces_frames below).
//
// Where it comes from:
//   - OpenGL calls: the OpenGL 3.3 specification (khronos.org). Apple marks OpenGL as old but still
//     ships it on every Mac.
//   - Dear ImGui's OpenGL 3 backend (imgui_impl_opengl3.cpp, MIT licence) draws ImGui's triangles.
//
// Only built off Windows: the whole file is inside `#ifndef _WIN32`. It is not tested on every
// change, because the person develops on Windows.

#ifndef _WIN32

#include "window/renderer.h"
#include "look/theme.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>      // Dear ImGui's GLFW input code
#include <backends/imgui_impl_opengl3.h>   // Dear ImGui's OpenGL drawing code

#include "window/gl.h"   // the system's OpenGL header

#include <algorithm>
#include <map>

namespace sessionscope {

namespace {   // private to this file

class GlRenderer final : public Renderer {
public:
    ~GlRenderer() override {
        for (const auto &entry : textures_) {
            const GLuint name = entry.second.name;
            glDeleteTextures(1, &name);
        }
        if (imgui_ready_) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
        }
    }

    bool init(GLFWwindow *window, std::string &) {
        window_ = window;
        glfwMakeContextCurrent(window);   // OpenGL calls from now on go to this window
        glfwSwapInterval(1);              // VSync: show each frame at the next screen refresh
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
        imgui_ready_ = true;
        return true;
    }

    void present_blank() override {
        clear();
        present();
    }

    void begin_frame() override { ImGui_ImplOpenGL3_NewFrame(); }

    void render() override {
        clear();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // For screenshots only.
    bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) override {
        glfwGetFramebufferSize(window_, &width, &height);
        if (width <= 0 || height <= 0) return false;
        std::vector<std::uint8_t> bottom_up(static_cast<std::size_t>(width) * height * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);   // rows are packed tightly, no padding
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bottom_up.data());
        // OpenGL reads the bottom row first; screenshots want the top row first, so flip.
        const std::size_t row = static_cast<std::size_t>(width) * 3;
        rgb.resize(bottom_up.size());
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottom_up.data() + (height - 1 - y) * row, row, rgb.data() + y * row);
        }
        return true;
    }

    void present() override { glfwSwapBuffers(window_); }

    // glfwSwapInterval(n): each swap waits for n screen refreshes (needs this window's context current).
    void set_frames_per_refresh(int refreshes) override {
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(refreshes < 1 ? 1 : refreshes);
    }

    // OpenGL has nothing to wait on before a frame, so hud_app.cpp does the waiting.
    bool paces_frames() const override { return false; }

    ImTextureID upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) override {
        if (width < 1 || height < 1) return existing;
        auto found = textures_.find(existing);
        if (found == textures_.end()) {
            Texture texture;
            glGenTextures(1, &texture.name);
            glBindTexture(GL_TEXTURE_2D, texture.name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   // smooth when shrunk
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);   // smooth when stretched
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);         // GL_CLAMP_TO_EDGE: no
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);         // wrapping at the edges
            const auto id = static_cast<ImTextureID>(texture.name);
            found = textures_.emplace(id, texture).first;
        }
        Texture &texture = found->second;
        glBindTexture(GL_TEXTURE_2D, texture.name);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texture.width == width && texture.height == height) {
            // Same size: overwrite the pixels in place (no new memory on the graphics card).
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            // New size: make new storage.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            texture.width = width;
            texture.height = height;
        }
        return found->first;
    }

    void destroy_texture(ImTextureID texture) override {
        const auto found = textures_.find(texture);
        if (found == textures_.end()) return;
        const GLuint name = found->second.name;
        glDeleteTextures(1, &name);
        textures_.erase(found);
    }

private:
    // Fills the window with the background colour.
    void clear() {
        int width, height;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(BACKGROUND_RGBA[0], BACKGROUND_RGBA[1], BACKGROUND_RGBA[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    struct Texture {
        GLuint name = 0;   // OpenGL's number for the texture
        int width = 0;
        int height = 0;
    };
    GLFWwindow *window_ = nullptr;
    bool imgui_ready_ = false;
    std::map<ImTextureID, Texture> textures_;
};

}  // namespace

void Renderer::set_window_hints() {
    // Ask GLFW for OpenGL 3.3 "core profile" (no old OpenGL 1.x functions); macOS requires
    // "forward compatible" for anything newer than OpenGL 2.1.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
}

std::unique_ptr<Renderer> Renderer::create(GLFWwindow *window, std::string &error) {
    auto renderer = std::make_unique<GlRenderer>();
    if (!renderer->init(window, error)) return nullptr;
    return renderer;
}

}  // namespace sessionscope

#endif  // !_WIN32
