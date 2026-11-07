// =================================================================================================
// menu.cpp - right-click menus (see menu.h)
// =================================================================================================
//
// Where it comes from: the Windows menu functions CreatePopupMenu, AppendMenuW and
// TrackPopupMenu (documented by Microsoft under "Menus"), and Dear ImGui's MenuItem/BeginMenu.

#include "window/menu.h"

#include <imgui.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

namespace sessionscope {

int draw_menu_with_imgui(const std::vector<MenuItem> &items) {
    int picked = 0;
    for (const MenuItem &item : items) {
        if (item.separator) {
            ImGui::Separator();
        } else if (!item.children.empty()) {
            if (ImGui::BeginMenu(item.label.c_str(), !item.disabled)) {
                if (const int id = draw_menu_with_imgui(item.children)) picked = id;
                ImGui::EndMenu();
            }
        } else if (ImGui::MenuItem(item.label.c_str(), nullptr, item.checked, !item.disabled)) {
            picked = item.id;
        }
    }
    return picked;
}

#ifdef _WIN32

namespace {

// Turns UTF-8 text (what the HUD uses) into UTF-16 (what Windows menus want).
std::wstring wide(const std::string &text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), length);
    return out;
}

// Builds a Windows menu from the items, submenus included.
HMENU build(const std::vector<MenuItem> &items) {
    HMENU menu = CreatePopupMenu();
    for (const MenuItem &item : items) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else if (!item.children.empty()) {
            // A submenu: Windows frees it together with its parent menu.
            AppendMenuW(menu, MF_POPUP | (item.disabled ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(build(item.children)),
                        wide(item.label).c_str());
        } else {
            const UINT flags = MF_STRING | (item.checked ? MF_CHECKED : 0) | (item.disabled ? MF_GRAYED : 0);
            AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), wide(item.label).c_str());
        }
    }
    return menu;
}

}  // namespace

bool native_menus_available() { return true; }

int show_native_menu(GLFWwindow *window, const std::vector<MenuItem> &items) {
    HWND owner = glfwGetWin32Window(window);
    HMENU menu = build(items);
    POINT mouse;
    GetCursorPos(&mouse);
    // Windows' rule for popup menus: the owner window must be in front, or the menu won't close
    // when the person clicks elsewhere (Microsoft's TrackPopupMenu documentation).
    SetForegroundWindow(owner);
    // TPM_RETURNCMD: return the picked id instead of sending it as a message. TPM_RIGHTBUTTON: the
    // right button can pick items too.
    const int picked = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, mouse.x, mouse.y, 0, owner, nullptr));
    DestroyMenu(menu);
    return picked;
}

#else

bool native_menus_available() { return false; }
int show_native_menu(GLFWwindow *, const std::vector<MenuItem> &) { return 0; }

#endif

}  // namespace sessionscope
