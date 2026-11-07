// Windows renderer: Direct3D 11 + DirectComposition (see renderer.h).
//
// The swap chain is a flip-model chain made for composition: two buffers that DWM shows directly,
// with no copy into the window's redirection surface. It is created with a frame-latency
// waitable object and a queue of one frame, and begin_frame() waits on that object: the frame is
// then built from the newest data just before the display needs it, and the CPU sleeps instead of
// spinning inside Present while the driver waits for the screen.

#ifdef _WIN32

#include "renderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_glfw.h>

#include <cstring>
#include <map>

namespace sessionscope {

namespace {

// Holds one COM reference and releases it when it goes out of scope.
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;

    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    // For functions that hand out a new reference: releases the old one first.
    T **put() {
        reset();
        return &ptr_;
    }
    void **put_void() { return reinterpret_cast<void **>(put()); }
    void reset() {
        if (ptr_) ptr_->Release();
        ptr_ = nullptr;
    }

private:
    T *ptr_ = nullptr;
};

constexpr DXGI_FORMAT BACK_BUFFER_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT BUFFER_COUNT = 2;
constexpr UINT SWAP_FLAGS = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

class D3D11Renderer final : public Renderer {
public:
    ~D3D11Renderer() override;

    bool init(GLFWwindow *window, std::string &error);

    void present_blank() override;
    void begin_frame() override;
    void render() override;
    bool read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) override;
    void present() override;
    bool paces_frames() const override { return true; }

    ImTextureID upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) override;
    void destroy_texture(ImTextureID texture) override;

private:
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

    GLFWwindow *window_ = nullptr;
    HWND hwnd_ = nullptr;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIFactory2> factory_;
    Com<IDXGISwapChain2> swap_chain_;
    Com<ID3D11RenderTargetView> render_target_;
    Com<IDCompositionDevice> composition_;
    Com<IDCompositionTarget> composition_target_;
    Com<IDCompositionVisual> visual_;
    HANDLE frame_ready_ = nullptr;   // signalled when the display can take another frame
    int buffer_width_ = 0;
    int buffer_height_ = 0;
    bool imgui_ready_ = false;
    std::map<ImTextureID, Texture> textures_;
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
}

bool D3D11Renderer::init(GLFWwindow *window, std::string &error) {
    window_ = window;
    hwnd_ = glfwGetWin32Window(window);
    glfwGetFramebufferSize(window, &buffer_width_, &buffer_height_);
    if (buffer_width_ < 1) buffer_width_ = 1;
    if (buffer_height_ < 1) buffer_height_ = 1;

    if (!create_device(error) || !create_swap_chain(error) || !create_composition(error)) return false;
    create_render_target();

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX11_Init(device_.get(), context_.get());
    imgui_ready_ = true;
    return true;
}

// The fastest graphics card in the machine (the discrete one on a laptop), else the default.
bool D3D11Renderer::create_device(std::string &error) {
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), factory_.put_void()))) {
        error = "Could not start DXGI (CreateDXGIFactory1 failed).";
        return false;
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

    Com<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory_->QueryInterface(__uuidof(IDXGIFactory6), factory6.put_void()))) {
        Com<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                               __uuidof(IDXGIAdapter1), adapter.put_void()) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            if (SUCCEEDED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                            ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), nullptr,
                                            context_.put()))) {
                return true;
            }
        }
    }
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                    D3D11_SDK_VERSION, device_.put(), nullptr, context_.put()))) {
        return true;
    }
    error = "No graphics card with Direct3D 11 (feature level 10.0 or newer) was found.";
    return false;
}

bool D3D11Renderer::create_swap_chain(std::string &error) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(buffer_width_);
    desc.Height = static_cast<UINT>(buffer_height_);
    desc.Format = BACK_BUFFER_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = BUFFER_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;                // while resizing, stretch the last frame
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;            // the HUD is opaque
    desc.Flags = SWAP_FLAGS;

    Com<IDXGISwapChain1> chain;
    if (FAILED(factory_->CreateSwapChainForComposition(device_.get(), &desc, nullptr, chain.put()))) {
        error = "Could not create the swap chain (CreateSwapChainForComposition failed).";
        return false;
    }
    if (FAILED(chain->QueryInterface(__uuidof(IDXGISwapChain2), swap_chain_.put_void()))) {
        error = "The swap chain has no frame-latency control (IDXGISwapChain2): Windows 8.1 or newer is needed.";
        return false;
    }
    swap_chain_->SetMaximumFrameLatency(1);   // one frame queued at most: the newest data, shown next
    frame_ready_ = swap_chain_->GetFrameLatencyWaitableObject();
    if (!frame_ready_) {
        error = "Could not get the swap chain's frame-latency waitable object.";
        return false;
    }
    return true;
}

// Puts the swap chain on the window through DirectComposition, above anything GDI might draw.
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

void D3D11Renderer::create_render_target() {
    Com<ID3D11Texture2D> back_buffer;
    swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void());
    device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put());
}

void D3D11Renderer::resize_if_needed() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width < 1 || height < 1) return;   // minimised: keep the old buffers
    if (width == buffer_width_ && height == buffer_height_) return;
    render_target_.reset();   // every reference to the buffers must go before resizing them
    context_->ClearState();
    if (SUCCEEDED(swap_chain_->ResizeBuffers(BUFFER_COUNT, static_cast<UINT>(width), static_cast<UINT>(height),
                                             DXGI_FORMAT_UNKNOWN, SWAP_FLAGS))) {
        buffer_width_ = width;
        buffer_height_ = height;
    }
    create_render_target();
}

void D3D11Renderer::present_blank() {
    WaitForSingleObjectEx(frame_ready_, 1000, TRUE);
    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(render_target_.get(), black);
    present();
}

void D3D11Renderer::begin_frame() {
    // Sleeps until the display has room for a frame (at most one refresh). Alertable, so window
    // messages delivered as APCs still run.
    WaitForSingleObjectEx(frame_ready_, 1000, TRUE);
    resize_if_needed();
    ImGui_ImplDX11_NewFrame();
}

void D3D11Renderer::render() {
    ID3D11RenderTargetView *target = render_target_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(target, black);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

bool D3D11Renderer::read_pixels(int &width, int &height, std::vector<std::uint8_t> &rgb) {
    Com<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void()))) return false;
    D3D11_TEXTURE2D_DESC desc;
    back_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.put()))) return false;
    context_->CopyResource(staging.get(), back_buffer.get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);
    rgb.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const auto *row = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        std::uint8_t *out = rgb.data() + static_cast<std::size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {   // the buffer is B, G, R, A
            out[x * 3] = row[x * 4 + 2];
            out[x * 3 + 1] = row[x * 4 + 1];
            out[x * 3 + 2] = row[x * 4];
        }
    }
    context_->Unmap(staging.get(), 0);
    return true;
}

void D3D11Renderer::present() {
    swap_chain_->Present(1, 0);   // shown at the next refresh
}

ImTextureID D3D11Renderer::upload_texture(ImTextureID existing, int width, int height, const std::uint32_t *rgba) {
    if (width < 1 || height < 1) return existing;
    auto found = textures_.find(existing);
    if (found == textures_.end() || found->second.width != width || found->second.height != height) {
        if (found != textures_.end()) destroy_texture(existing);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;   // rewritten often, from the CPU
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Texture texture;
        texture.width = width;
        texture.height = height;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture.texture))) return 0;
        if (FAILED(device_->CreateShaderResourceView(texture.texture, nullptr, &texture.view))) {
            texture.texture->Release();
            return 0;
        }
        const auto id = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.view));
        found = textures_.emplace(id, texture).first;
    }

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

void Renderer::set_window_hints() {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // no OpenGL context: Direct3D draws instead
}

std::unique_ptr<Renderer> Renderer::create(GLFWwindow *window, std::string &error) {
    auto renderer = std::make_unique<D3D11Renderer>();
    if (!renderer->init(window, error)) return nullptr;
    return renderer;
}

}  // namespace sessionscope

#endif  // _WIN32
