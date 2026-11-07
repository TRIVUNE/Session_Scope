// macOS (and any non-Windows) renderer: OpenGL 3.3 core with VSync (see renderer.h).

#ifndef _WIN32

#include "renderer.h"
#include "theme.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "gl.h"

#include <algorithm>
#include <map>

namespace sessionscope {

namespace {

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
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);   // VSync on
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330 core");
        imgui_ready_ = true;
        return true;
    }

    void present_blank() override {
        glClearColor(BACKGROUND_RGBA[0], BACKGROUND_RGBA[1], BACKGROUND_RGBA[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        present();
    }

    void begin_frame() override { ImGui_ImplOpenGL3_NewFrame(); }

    void render() override {
        int width, height;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(BACKGROUND_RGBA[0], BACKGROUND_RGBA[1], BACKGROUND_RGBA[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) override {
        glfwGetFramebufferSize(window_, &width, &height);
        if (width <= 0 || height <= 0) return false;
        std::vector<std::uint8_t> bottom_up(static_cast<std::size_t>(width) * height * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, bottom_up.data());
        // OpenGL reads bottom-up; callers want rows top-down.
        const std::size_t row = static_cast<std::size_t>(width) * 3;
        rgb.resize(bottom_up.size());
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottom_up.data() + (height - 1 - y) * row, row, rgb.data() + y * row);
        }
        return true;
    }

    void present() override { glfwSwapBuffers(window_); }

    bool paces_frames() const override { return false; }

    ImTextureID upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) override {
        if (width < 1 || height < 1) return existing;
        auto found = textures_.find(existing);
        if (found == textures_.end()) {
            Texture texture;
            glGenTextures(1, &texture.name);
            glBindTexture(GL_TEXTURE_2D, texture.name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);   // GL_CLAMP_TO_EDGE
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
            const auto id = static_cast<ImTextureID>(texture.name);
            found = textures_.emplace(id, texture).first;
        }
        glBindTexture(GL_TEXTURE_2D, found->second.name);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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
    struct Texture {
        GLuint name = 0;
    };
    GLFWwindow *window_ = nullptr;
    bool imgui_ready_ = false;
    std::map<ImTextureID, Texture> textures_;
};

}  // namespace

void Renderer::set_window_hints() {
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
