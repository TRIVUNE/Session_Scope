#ifndef SESSIONSCOPE_HUD_RENDERER_H
#define SESSIONSCOPE_HUD_RENDERER_H

// The part of the HUD window that talks to the graphics card: one implementation per system.
//
//   Windows (renderer_d3d11.cpp): Direct3D 11 drawing into a DirectComposition flip swap chain
//     with a frame-latency waitable object and a one-frame queue. Each frame starts the moment
//     the display can take it, so motion is smooth, input lag is one frame, and no core spins
//     while the driver waits for the screen. The same setup MiniMeters uses.
//   macOS (renderer_gl.cpp): OpenGL 3.3 with VSync, as before.
//
// GLFW still owns the window and the input on both systems; Dear ImGui draws through whichever
// backend is in use.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

struct GLFWwindow;

namespace sessionscope {

// Pictures the view draws with ImDrawList::AddImage, such as the core heatmap.
class TextureStore {
public:
    virtual ~TextureStore() = default;
    // Puts `width` x `height` pixels (bytes R, G, B, A, rows top to bottom) on the graphics card.
    // Reuses `existing` when it has the same size, else replaces it. Returns the texture to draw;
    // 0 if it could not be made. Textures are stretched smoothly (linear filtering).
    virtual ImTextureID upload_texture(ImTextureID existing, int width, int height,
                                       const std::uint32_t *rgba) = 0;
    virtual void destroy_texture(ImTextureID texture) = 0;
};

class Renderer : public TextureStore {
public:
    // GLFW window hints this renderer needs. Call after glfwInit, before glfwCreateWindow.
    static void set_window_hints();
    // Sets up drawing into `window` and Dear ImGui's backends (the ImGui context must exist).
    // Returns null and sets `error` if the graphics card can't do it.
    static std::unique_ptr<Renderer> create(GLFWwindow *window, std::string &error);

    ~Renderer() override = default;

    // Shows a plain black frame without ImGui: before the window first appears, so it never
    // flashes white, and before the view has loaded its fonts.
    virtual void present_blank() = 0;
    // Start of a frame: waits until the display can take one (where the system offers that), then
    // starts ImGui's renderer frame.
    virtual void begin_frame() = 0;
    // Draws ImGui's draw data over a black background.
    virtual void render() = 0;
    // The frame just rendered, as RGB rows top to bottom. Call between render() and present().
    virtual bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) = 0;
    // Shows the frame, in step with the display.
    virtual void present() = 0;
    // True if begin_frame() itself waits for the display, so the caller needn't pace frames.
    virtual bool paces_frames() const = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_RENDERER_H
