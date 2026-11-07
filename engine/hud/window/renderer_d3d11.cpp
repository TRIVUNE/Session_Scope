// =================================================================================================
// renderer_d3d11.cpp - drawing on Windows: Direct3D 11 + DirectComposition
// =================================================================================================
//
// What this does, in plain words:
//   1. Picks the graphics card (GPU) that drives the screen the window is on.
//   2. Makes a "swap chain": two pictures (buffers) of the window. We draw into one while the
//      screen shows the other, then they swap. This is the "flip model".
//   3. Hands the swap chain to DirectComposition, the part of Windows that puts windows together
//      on screen, so our frames go straight to the screen without an extra copy.
//   4. Asks Windows for a "frame-latency waitable object": something we can wait on that says "the
//      screen is ready for your next frame". We wait on it at the start of every frame, so:
//        - the frame is drawn from the newest data, just before it's needed (low delay),
//        - the processor sleeps while waiting instead of spinning (low CPU),
//        - every screen refresh gets exactly one frame (smooth motion).
//
// Where it comes from:
//   - Steps 2-4 are the setup MiniMeters uses. We read it from MiniMeters.exe with REA and Ghidra
//     (decompiled function FUN_140166990; see handoff/minimeters.md). The code below is our own.
//   - The API calls are Microsoft's, documented at learn.microsoft.com under "DXGI flip model",
//     "IDXGISwapChain2::GetFrameLatencyWaitableObject" and "DirectComposition".
//   - Dear ImGui's Direct3D 11 backend (imgui_impl_dx11.cpp, MIT licence) draws ImGui's triangles.
//
// Step 1 differs from MiniMeters: it asks for the "high performance" GPU. On a laptop with two GPUs
// that is the gaming GPU, but the screen is usually wired to the other one, so every frame has to be
// copied across. Measured on this machine (GTX 1650 + Radeon, 120 Hz screen, demo running):
//     high-performance GPU:  ~52 frames per second, ~14 ms per present, 15-17 % of one core
//     the screen's own GPU:  ~120 frames per second, ~0.3 ms per present, ~6 % per 60 frames
// so we use the GPU the screen is on.
//
// Only built on Windows: the whole file is inside `#ifdef _WIN32`.

#ifdef _WIN32

#include "window/renderer.h"
#include "look/theme.h"

// windows.h defines `min` and `max` as macros unless told not to; they break std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN   // leave out rarely used parts of windows.h: faster to compile
#include <windows.h>
#include <d3d11.h>     // Direct3D 11: drawing
#include <dcomp.h>     // DirectComposition: putting our frames on screen
#include <dxgi1_6.h>   // DXGI: graphics cards, screens and swap chains

// GLFW made the window; this lets us ask it for the Windows handle (HWND) of that window.
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <backends/imgui_impl_dx11.h>   // Dear ImGui's Direct3D 11 drawing code
#include <backends/imgui_impl_glfw.h>   // Dear ImGui's GLFW input code (mouse, keyboard)

#include <cstring>
#include <map>

namespace sessionscope {

namespace {   // everything in here is private to this file

// -------------------------------------------------------------------------------------------------
// Com<T>: a small helper for Windows "COM" objects.
//
// Direct3D objects count how many places use them. Each place must call Release() when done, or
// the memory is never freed. Com<T> calls Release() for us when it goes out of scope, so we can't
// forget. (Microsoft's own version of this is called ComPtr; this one is just smaller.)
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com &) = delete;              // no copying: two copies would both call Release()
    Com &operator=(const Com &) = delete;

    T *get() const { return ptr_; }         // the object itself, to pass to Windows functions
    T *operator->() const { return ptr_; }  // lets us write com->Function(), like a pointer
    explicit operator bool() const { return ptr_ != nullptr; }

    // For Windows functions that "hand out" an object through a pointer-to-pointer argument.
    // Releases what we held before, then gives Windows the place to put the new one.
    T **put() {
        reset();
        return &ptr_;
    }
    void **put_void() { return reinterpret_cast<void **>(put()); }   // the same, for void** arguments

    void reset() {
        if (ptr_) ptr_->Release();
        ptr_ = nullptr;
    }

private:
    T *ptr_ = nullptr;
};

// Settings for the swap chain, used both when it is made and when it is resized.
constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;   // 8 bits each of blue, green, red, alpha
constexpr UINT BUFFER_COUNT = 2;                                         // two pictures: one shown, one drawn
constexpr UINT SWAP_FLAGS = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;   // "give me something to wait on"

// -------------------------------------------------------------------------------------------------
// The renderer itself.
class D3D11Renderer final : public Renderer {
public:
    ~D3D11Renderer() override;

    bool init(GLFWwindow *window, std::string &error);

    // The functions renderer.h asks for.
    void present_blank() override;
    void begin_frame() override;
    void render() override;
    bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) override;
    void present() override;
    bool paces_frames() const override { return true; }

    ImTextureID upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) override;
    void destroy_texture(ImTextureID texture) override;

private:
    // A picture on the graphics card, and the "view" Direct3D needs to read it while drawing.
    struct Texture {
        ID3D11Texture2D *texture = nullptr;
        ID3D11ShaderResourceView *view = nullptr;
        int width = 0;
        int height = 0;
    };

    bool create_device(std::string &error);
    bool create_swap_chain(std::string &error);
    bool create_composition(std::string &error);
    void create_render_target();
    void resize_if_needed();
    void wait_for_screen();

    GLFWwindow *window_ = nullptr;
    HWND hwnd_ = nullptr;                          // the window, as Windows knows it
    Com<ID3D11Device> device_;                     // the graphics card, for making things
    Com<ID3D11DeviceContext> context_;             // the graphics card, for drawing things
    Com<IDXGIFactory2> factory_;                   // makes swap chains, lists graphics cards
    Com<IDXGISwapChain2> swap_chain_;              // the two pictures of the window
    Com<ID3D11RenderTargetView> render_target_;    // "draw into the swap chain's back picture"
    Com<IDCompositionDevice> composition_;         // DirectComposition, below
    Com<IDCompositionTarget> composition_target_;  // ... attached to our window
    Com<IDCompositionVisual> visual_;              // ... showing our swap chain
    HANDLE frame_ready_ = nullptr;                 // signalled when the screen can take a new frame
    int buffer_width_ = 0;                         // the swap chain's current size, in pixels
    int buffer_height_ = 0;
    bool imgui_ready_ = false;
    std::map<ImTextureID, Texture> textures_;      // every texture we made, by the id ImGui uses
};

D3D11Renderer::~D3D11Renderer() {
    for (auto &entry : textures_) {
        if (entry.second.view) entry.second.view->Release();
        if (entry.second.texture) entry.second.texture->Release();
    }
    if (imgui_ready_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    if (frame_ready_) CloseHandle(frame_ready_);
    // Everything held in Com<...> members is released automatically after this.
}

bool D3D11Renderer::init(GLFWwindow *window, std::string &error) {
    window_ = window;
    hwnd_ = glfwGetWin32Window(window);
    glfwGetFramebufferSize(window, &buffer_width_, &buffer_height_);
    if (buffer_width_ < 1) buffer_width_ = 1;   // a swap chain can't be 0 pixels wide
    if (buffer_height_ < 1) buffer_height_ = 1;

    // Each step needs the one before; stop at the first that fails.
    if (!create_device(error) || !create_swap_chain(error) || !create_composition(error)) return false;
    create_render_target();

    ImGui_ImplGlfw_InitForOther(window, true);           // ImGui reads mouse and keys from GLFW...
    ImGui_ImplDX11_Init(device_.get(), context_.get());  // ...and draws with Direct3D 11
    imgui_ready_ = true;
    return true;
}

// Step 1: the graphics card. We want the one wired to the screen the window is on (see the top of
// this file for why). Windows lists graphics cards as "adapters", and each adapter's screens as
// "outputs"; we look for the output whose monitor is the window's monitor.
bool D3D11Renderer::create_device(std::string &error) {
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), factory_.put_void()))) {
        error = "Could not start DXGI (CreateDXGIFactory1 failed).";
        return false;
    }
    // BGRA_SUPPORT: needed by DirectComposition. SINGLETHREADED: we only draw from one thread, so
    // Direct3D can skip its internal locks (a little faster).
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    // The Direct3D versions we can work with, best first. 10.0 covers graphics cards from 2008 on.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

    // Which monitor is the window on?
    const HMONITOR window_monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);

    Com<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory_->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
        Com<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != window_monitor) continue;
            // Found the graphics card that drives our screen. D3D_DRIVER_TYPE_UNKNOWN is required
            // whenever a specific adapter is passed in.
            if (SUCCEEDED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                            ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), nullptr,
                                            context_.put()))) {
                return true;
            }
        }
    }
    // Not found (unusual): let Windows pick its default graphics card.
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                    D3D11_SDK_VERSION, device_.put(), nullptr, context_.put()))) {
        return true;
    }
    error = "No graphics card with Direct3D 11 (feature level 10.0 or newer) was found.";
    return false;
}

// Step 2: the swap chain, with the "something to wait on" (step 4) switched on.
bool D3D11Renderer::create_swap_chain(std::string &error) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};   // = {} sets every field to 0 first
    desc.Width = static_cast<UINT>(buffer_width_);
    desc.Height = static_cast<UINT>(buffer_height_);
    desc.Format = BACK_BUFFER_FORMAT;
    desc.SampleDesc.Count = 1;                          // no anti-aliasing at this level
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // we draw into it
    desc.BufferCount = BUFFER_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;                // while resizing, stretch the last frame
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // the flip model
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;            // the HUD is not see-through
    desc.Flags = SWAP_FLAGS;

    Com<IDXGISwapChain1> chain;
    // "ForComposition": a swap chain made for DirectComposition (step 3), not tied to the window yet.
    if (FAILED(factory_->CreateSwapChainForComposition(device_.get(), &desc, nullptr, chain.put()))) {
        error = "Could not create the swap chain (CreateSwapChainForComposition failed).";
        return false;
    }
    // IDXGISwapChain2 is the newer interface that has the waitable object (Windows 8.1 and later).
    if (FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain2), swap_chain_.put_void()))) {
        error = "The swap chain has no frame-latency control (IDXGISwapChain2): Windows 8.1 or newer is needed.";
        return false;
    }
    // At most one frame waiting to be shown: the screen always gets our newest frame, never an old
    // one stuck in a queue.
    swap_chain_->SetMaximumFrameLatency(1);
    frame_ready_ = swap_chain_->GetFrameLatencyWaitableObject();
    if (!frame_ready_) {
        error = "Could not get the swap chain's frame-latency waitable object.";
        return false;
    }
    return true;
}

// Step 3: attach the swap chain to our window through DirectComposition.
//   device  -> makes the pieces
//   target  -> "this window"; TRUE = above anything else drawn in the window
//   visual  -> "show this swap chain"
//   Commit  -> apply all of the above at once
bool D3D11Renderer::create_composition(std::string &error) {
    Com<IDXGIDevice> dxgi_device;
    device_->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void());
    if (FAILED(DCompositionCreateDevice(dxgi_device.get(), __uuidof(IDCompositionDevice), composition_.put_void())) ||
        FAILED(composition_->CreateTargetForHwnd(hwnd_, TRUE, composition_target_.put())) ||
        FAILED(composition_->CreateVisual(visual_.put())) ||
        FAILED(visual_->SetContent(swap_chain_.get())) ||
        FAILED(composition_target_->SetRoot(visual_.get())) ||
        FAILED(composition_->Commit())) {
        error = "Could not attach the swap chain to the window (DirectComposition failed).";
        return false;
    }
    return true;
}

// "Draw into the swap chain's back picture": Direct3D needs this view of buffer 0 to draw into it.
void D3D11Renderer::create_render_target() {
    Com<ID3D11Texture2D> back_buffer;
    swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void());
    device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put());
}

// The window changed size: the swap chain's pictures must change size with it.
void D3D11Renderer::resize_if_needed() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width < 1 || height < 1) return;   // minimised: keep the old pictures
    if (width == buffer_width_ && height == buffer_height_) return;   // nothing changed (the usual case)
    render_target_.reset();   // Windows refuses to resize while anything still points at the pictures
    context_->ClearState();
    if (SUCCEEDED(swap_chain_->ResizeBuffers(BUFFER_COUNT, static_cast<UINT>(width), static_cast<UINT>(height),
                                             DXGI_FORMAT_UNKNOWN, SWAP_FLAGS))) {
        buffer_width_ = width;
        buffer_height_ = height;
    }
    create_render_target();
}

// Step 4 in action: sleep until the screen is ready for a new frame (at most one screen refresh,
// so about 8 ms at 120 Hz). The 1000 ms limit only matters if something went wrong. TRUE lets
// Windows run queued window messages while we wait.
void D3D11Renderer::wait_for_screen() { WaitForSingleObjectEx(frame_ready_, 1000, TRUE); }

void D3D11Renderer::present_blank() {
    wait_for_screen();
    context_->ClearRenderTargetView(render_target_.get(), BACKGROUND_RGBA);
    present();
}

void D3D11Renderer::begin_frame() {
    wait_for_screen();
    resize_if_needed();
    ImGui_ImplDX11_NewFrame();
}

void D3D11Renderer::render() {
    ID3D11RenderTargetView *target = render_target_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);           // draw into the back picture
    context_->ClearRenderTargetView(target, BACKGROUND_RGBA);    // start from the background colour
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());         // then everything the modules drew
}

// For screenshots only (not every frame): copies the back picture somewhere the processor can read.
bool D3D11Renderer::read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) {
    Com<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void()))) return false;
    D3D11_TEXTURE2D_DESC desc;
    back_buffer->GetDesc(&desc);
    // A "staging" texture: memory the processor can read. The graphics card copies into it.
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.put()))) return false;
    context_->CopyResource(staging.get(), back_buffer.get());

    D3D11_MAPPED_SUBRESOURCE mapped;   // "where in memory the pixels are now"
    if (FAILED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        // Rows can have padding at the end, so step by RowPitch (the real row length in bytes).
        const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        std::uint8_t *out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {   // the swap chain stores blue, green, red, alpha; PNG wants red, green, blue
            out[x * 3] = row[x * 4 + 2];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4];
        }
    }
    context_->Unmap(staging.get(), 0);
    return true;
}

// Show the frame at the next screen refresh (the 1 means "wait for 1 refresh"; that's VSync).
void D3D11Renderer::present() { swap_chain_->Present(1, 0); }

// Puts a picture on the graphics card (see renderer.h). Called by the heatmap about ten times a
// second, so it reuses the texture when the size hasn't changed.
ImTextureID D3D11Renderer::upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) {
    if (width < 1 || height < 1) return existing;
    auto found = textures_.find(existing);
    const bool must_make_new = found == textures_.end() || found->second.width != width || found->second.height != height;
    if (must_make_new) {
        if (found != textures_.end()) destroy_texture(existing);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;                          // no smaller copies: we never shrink it much
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;    // red, green, blue, alpha: 1 byte each
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;            // rewritten often by the processor
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // drawn from, never drawn into
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Texture texture;
        texture.width = width;
        texture.height = height;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture.texture))) return 0;
        if (FAILED(device_->CreateShaderResourceView(texture.texture, nullptr, &texture.view))) {
            texture.texture->Release();
            return 0;
        }
        // Dear ImGui's DX11 code expects the id to be the "view" pointer, so we use that as the id.
        const auto id = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.view));
        found = textures_.emplace(id, texture).first;
    }

    // Copy the pixels in. WRITE_DISCARD: "I'm replacing all of it", which lets the graphics card
    // keep drawing the old picture while we write the new one (no waiting).
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context_->Map(found->second.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        for (int y = 0; y < height; ++y) {
            std::memcpy(static_cast<std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                        rgba + static_cast<std::size_t>(y) * width, static_cast<std::size_t>(width) * 4);
        }
        context_->Unmap(found->second.texture, 0);
    }
    return found->first;
}

void D3D11Renderer::destroy_texture(ImTextureID texture) {
    const auto found = textures_.find(texture);
    if (found == textures_.end()) return;
    found->second.view->Release();
    found->second.texture->Release();
    textures_.erase(found);
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// The two "static" functions from renderer.h, Windows versions.

void Renderer::set_window_hints() {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // GLFW must not make an OpenGL context: Direct3D draws instead
}

std::unique_ptr<Renderer> Renderer::create(GLFWwindow *window, std::string &error) {
    auto renderer = std::make_unique<D3D11Renderer>();
    if (!renderer->init(window, error)) return nullptr;
    return renderer;
}

}  // namespace sessionscope

#endif  // _WIN32
