// =================================================================================================
// desktop.cpp - tray icon, hotkey, taskbar button (see desktop.h)
// =================================================================================================
//
// How the Windows part works: Windows talks to programs with "messages" sent to windows. The tray
// icon and the hotkey both send theirs to a window we name, so we make a small window that is never
// shown, just to receive them. GLFW's glfwPollEvents() hands out the messages of every window in
// the program, including this one, so no extra loop is needed: its message function (window_proc
// below) runs inside glfwPollEvents and notes what happened in `events`; the main loop collects
// that with take_events().

#include "window/desktop.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // Shell_NotifyIconW
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace sessionscope {

#ifdef _WIN32

namespace {

constexpr UINT TRAY_MESSAGE = WM_APP + 1;   // the message number the tray icon sends us
constexpr UINT TRAY_ID = 1;                 // our one tray icon
constexpr int HOTKEY_ID = 1;                // our one hotkey
constexpr int MENU_SHOW_HIDE = 1, MENU_QUIT = 2;

// Draws the tray icon: 32 x 32 pixels, a dark blue square with five little bars in the heatmap's
// colours, like a tiny version of the HUD. Made in code so the program needs no icon file.
HICON make_icon() {
    constexpr int SIZE = 32;
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = SIZE;
    header.bV5Height = -SIZE;   // negative: rows from top to bottom
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void *bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour) return LoadIcon(nullptr, IDI_APPLICATION);   // fall back to Windows' plain icon

    auto *pixels = static_cast<std::uint32_t *>(bits);   // 0xAARRGGBB
    const std::uint32_t bar_colours[5] = {0xff0014ff, 0xff00c8ff, 0xff14e65a, 0xfffafa00, 0xffe10000};   // blue..red
    const int bar_heights[5] = {10, 16, 22, 18, 26};
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            std::uint32_t c = 0xff00006e;   // dark blue background
            const int bar = (x - 3) / 5;    // five bars, 4 pixels wide, 1 apart, from x = 3
            if (x >= 3 && bar < 5 && (x - 3) % 5 < 4 && y >= 29 - bar_heights[bar] && y < 29) c = bar_colours[bar];
            pixels[y * SIZE + x] = c;
        }
    }
    HBITMAP mask = CreateBitmap(SIZE, SIZE, 1, 1, nullptr);   // unused with alpha, but required
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

// Reads "ctrl+alt+h" into Windows' modifier flags and key code. False if it isn't one.
bool parse_hotkey(const std::string &text, UINT &modifiers, UINT &key, std::string &label) {
    modifiers = 0;
    key = 0;
    label.clear();
    std::string part;
    std::vector<std::string> parts;
    for (const char ch : text + "+") {   // split at '+'
        if (ch == '+') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string &p = parts[i];
        const bool last = i + 1 == parts.size();
        std::string shown;
        if (!last && p == "ctrl") { modifiers |= MOD_CONTROL; shown = "Ctrl"; }
        else if (!last && p == "alt") { modifiers |= MOD_ALT; shown = "Alt"; }
        else if (!last && p == "shift") { modifiers |= MOD_SHIFT; shown = "Shift"; }
        else if (!last && p == "win") { modifiers |= MOD_WIN; shown = "Win"; }
        else if (last && p.size() == 1 && std::isalnum(static_cast<unsigned char>(p[0]))) {
            key = static_cast<UINT>(std::toupper(static_cast<unsigned char>(p[0])));   // 'A'-'Z', '0'-'9' are their own key codes
            shown = std::string(1, static_cast<char>(key));
        } else if (last && p.size() >= 2 && p[0] == 'f') {   // f1 .. f24
            const int n = std::atoi(p.c_str() + 1);
            if (n < 1 || n > 24) return false;
            key = static_cast<UINT>(VK_F1 + n - 1);
            shown = "F" + std::to_string(n);
        } else {
            return false;
        }
        label += (label.empty() ? "" : "+") + shown;
    }
    return key != 0 && modifiers != 0;   // a bare key would steal it from every program
}

}  // namespace

// The Windows parts, kept out of desktop.h so the rest of the HUD doesn't need windows.h.
struct Desktop::Impl {
    HWND hwnd = nullptr;                 // the hidden window that receives the messages
    HICON icon = nullptr;
    UINT taskbar_created = 0;            // Explorer's "the taskbar was (re)started" message
    bool hotkey_registered = false;
    bool tray_on = false;
    Desktop::Events events;

    // Puts the icon in the notification area, or removes it.
    void tray(bool on) {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = TRAY_ID;
        if (on) {
            data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            data.uCallbackMessage = TRAY_MESSAGE;
            data.hIcon = icon;
            wcscpy_s(data.szTip, L"SessionScope - click to show or hide");
            Shell_NotifyIconW(NIM_ADD, &data);
        } else {
            Shell_NotifyIconW(NIM_DELETE, &data);
        }
        tray_on = on;
    }

    // Right-click on the tray icon: a small Windows menu.
    void tray_menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MENU_SHOW_HIDE, L"Show / hide HUD");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MENU_QUIT, L"Quit SessionScope");
        POINT mouse;
        GetCursorPos(&mouse);
        SetForegroundWindow(hwnd);   // Windows' rule for popup menus (see menu.cpp)
        const int picked = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, mouse.x, mouse.y, 0, hwnd, nullptr));
        DestroyMenu(menu);
        if (picked == MENU_SHOW_HIDE) events.toggle = true;
        if (picked == MENU_QUIT) events.quit = true;
    }

    // Windows calls this for every message to our hidden window.
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            if (message == WM_HOTKEY && wparam == HOTKEY_ID) {
                self->events.toggle = true;
                return 0;
            }
            if (message == TRAY_MESSAGE) {
                if (LOWORD(lparam) == WM_LBUTTONUP) self->events.toggle = true;
                if (LOWORD(lparam) == WM_RBUTTONUP) self->tray_menu();
                return 0;
            }
            if (message == self->taskbar_created && self->tray_on) {   // Explorer restarted: put the icon back
                self->tray(true);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

Desktop::Desktop() : impl_(std::make_unique<Impl>()) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = Impl::window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"SessionScopeDesktop";
    RegisterClassW(&window_class);   // fails harmlessly if already registered
    // A plain hidden window (not "message-only": popup menus need a window that can come to front).
    impl_->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"SessionScopeDesktop", L"SessionScope", WS_POPUP, 0, 0, 0, 0, nullptr,
                                  nullptr, instance, nullptr);
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
    impl_->icon = make_icon();
    impl_->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
}

Desktop::~Desktop() {
    if (impl_->tray_on) impl_->tray(false);
    if (impl_->hotkey_registered) UnregisterHotKey(impl_->hwnd, HOTKEY_ID);
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->icon) DestroyIcon(impl_->icon);
}

bool Desktop::available() { return true; }

void Desktop::set_tray_icon(bool on) {
    if (on != impl_->tray_on) impl_->tray(on);
    tray_on_ = on;
}

bool Desktop::set_hotkey(const std::string &text) {
    if (impl_->hotkey_registered) UnregisterHotKey(impl_->hwnd, HOTKEY_ID);
    impl_->hotkey_registered = false;
    UINT modifiers = 0, key = 0;
    std::string label;
    if (!parse_hotkey(text, modifiers, key, label)) {
        hotkey_label_ = "\"" + text + "\" is not a key combination";
        return false;
    }
    // MOD_NOREPEAT: holding the keys down toggles once, not over and over.
    impl_->hotkey_registered = RegisterHotKey(impl_->hwnd, HOTKEY_ID, modifiers | MOD_NOREPEAT, key) != 0;
    hotkey_label_ = impl_->hotkey_registered ? label : label + " (used by another program)";
    return impl_->hotkey_registered;
}

Desktop::Events Desktop::take_events() {
    const Events out = impl_->events;
    impl_->events = Events{};
    return out;
}

void Desktop::set_in_taskbar(GLFWwindow *window, bool in_taskbar) {
    // A "tool window" gets no taskbar button. Windows only notices a change of style while the
    // window is hidden, so hide it, change the style, show it again.
    HWND hwnd = glfwGetWin32Window(window);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR wanted = in_taskbar ? (style & ~WS_EX_TOOLWINDOW) : (style | WS_EX_TOOLWINDOW);
    if (wanted == style) return;
    const bool visible = IsWindowVisible(hwnd) != 0;
    if (visible) ShowWindow(hwnd, SW_HIDE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted);
    if (visible) ShowWindow(hwnd, SW_SHOW);
}

#else   // macOS and others: nothing yet

struct Desktop::Impl {};
Desktop::Desktop() = default;
Desktop::~Desktop() = default;
bool Desktop::available() { return false; }
void Desktop::set_tray_icon(bool on) { tray_on_ = on; }
bool Desktop::set_hotkey(const std::string &) { return false; }
Desktop::Events Desktop::take_events() { return {}; }
void Desktop::set_in_taskbar(GLFWwindow *, bool) {}

#endif

}  // namespace sessionscope
