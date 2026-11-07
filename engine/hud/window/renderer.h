#ifndef SESSIONSCOPE_HUD_RENDERER_H
#define SESSIONSCOPE_HUD_RENDERER_H

// =================================================================================================
// renderer.h - the part of the HUD that talks to the graphics card
// =================================================================================================
//
// Drawing on screen works differently on each operating system, so this file only says WHAT a
// renderer must do. Each system has its own file that says HOW:
//
//   Windows  window/renderer_d3d11.cpp   Direct3D 11 + DirectComposition: the smoothest option on
//                                        Windows. Copied in spirit from MiniMeters, whose setup we
//                                        read with REA (see handoff/minimeters.md).
//   macOS    window/renderer_gl.cpp      OpenGL 3.3, which macOS still ships.
//
// The build script compiles both files everywhere; each one is wrapped in `#ifdef _WIN32` (or
// `#ifndef _WIN32`), so only the right one turns into code.
//
// Who does what, from the bottom up:
//   GLFW         (third-party library) makes the window and tells us about the mouse and keys
//   Renderer     (this file) puts finished frames on screen, through the graphics card
//   Dear ImGui   (third-party library) turns "draw a box here, text there" into triangles
//   HudView      (hud_view.h) decides which boxes and text, using the modules

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>   // ImTextureID: how Dear ImGui refers to a picture on the graphics card

struct GLFWwindow;   // "there is a type called GLFWwindow"; GLFW's own header defines it

namespace sessionscope {

// Pictures the modules draw with, such as the core heatmap. A texture is a picture stored on the
// graphics card, so drawing it costs almost nothing.
class TextureStore {
public:
    virtual ~TextureStore() = default;

    // Puts `width` x `height` pixels on the graphics card. Each pixel is 4 bytes (red, green, blue,
    // alpha), rows from top to bottom. Pass the texture you got last time as `existing` to reuse
    // it (faster than making a new one when the size is the same). Returns the texture to draw,
    // or 0 if it could not be made. Pictures are stretched smoothly when drawn larger.
    virtual ImTextureID upload_texture(ImTextureID existing, int width, int height,
                                       const std::uint32_t *rgba) = 0;
    // Frees a texture's memory on the graphics card.
    virtual void destroy_texture(ImTextureID texture) = 0;
};

// `virtual ... = 0` means "every renderer must write this function"; renderer_d3d11.cpp and
// renderer_gl.cpp each do.
class Renderer : public TextureStore {
public:
    // Settings GLFW needs before it makes the window (for example "no OpenGL" on Windows).
    // Call after glfwInit, before glfwCreateWindow.
    static void set_window_hints();
    // Sets up drawing into `window`, and Dear ImGui's drawing code (the ImGui context must exist).
    // Returns nothing (nullptr) and fills `error` if the graphics card can't do it.
    static std::unique_ptr<Renderer> create(GLFWwindow *window, std::string &error);

    ~Renderer() override = default;

    // Shows one plain background-coloured frame, without ImGui. Used once, before the window first
    // appears, so it never flashes black or white.
    virtual void present_blank() = 0;
    // Start of a frame. On Windows this WAITS until the screen is ready for a new frame: that wait
    // is what keeps motion smooth and the processor idle in between.
    virtual void begin_frame() = 0;
    // Clears the window to the background colour and draws everything ImGui collected this frame.
    virtual void render() = 0;
    // Copies the frame just drawn into `rgb` (3 bytes per pixel, rows top to bottom), for
    // screenshots. Call between render() and present().
    virtual bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) = 0;
    // Puts the frame on screen at the next screen refresh.
    virtual void present() = 0;
    // True if begin_frame() already waits for the screen (Windows), so nobody else needs to sleep.
    virtual bool paces_frames() const = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_RENDERER_H
