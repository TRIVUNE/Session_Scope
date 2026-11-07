#ifndef SESSIONSCOPE_HUD_MENU_H
#define SESSIONSCOPE_HUD_MENU_H

// =================================================================================================
// menu.h - right-click menus, described once, shown by Windows or by Dear ImGui
// =================================================================================================
//
// The HUD's window is a thin strip, often only 150 pixels tall. A menu drawn inside the window
// (Dear ImGui's popups) gets cut off at the window's edge. So on Windows the menu is a real Windows
// menu (TrackPopupMenu), which can reach past the window like any program's right-click menu.
// MiniMeters does the same: its menus are their own windows ("wl::menu", seen with REA).
//
// The code that builds a menu doesn't care which kind shows it: it fills in MenuItems with numbers
// (ids), shows them, and gets back the id of what was clicked.
//
// On macOS there is no native version here yet, so the same items are drawn with Dear ImGui
// (draw_menu_with_imgui) and may be cut off by a small window.

#include <string>
#include <vector>

struct GLFWwindow;

namespace sessionscope {

struct MenuItem {
    std::string label;               // what the person reads
    int id = 0;                      // what comes back when it's clicked; 0 = nothing to do
    bool checked = false;            // a tick in front
    bool disabled = false;           // greyed out, can't be clicked (information lines)
    bool separator = false;          // a dividing line instead of an item
    std::vector<MenuItem> children;  // non-empty: a submenu that opens to the side
};

// A dividing line.
inline MenuItem menu_separator() {
    MenuItem item;
    item.separator = true;
    return item;
}

// True where show_native_menu works (Windows).
bool native_menus_available();

// Shows `items` as a Windows menu at the mouse and waits until the person picks something or
// clicks away. Returns the picked item's id, or 0. The program pauses while the menu is open, like
// any Windows program's menu.
int show_native_menu(GLFWwindow *window, const std::vector<MenuItem> &items);

// Draws `items` inside an open ImGui popup; returns the id clicked this frame, or 0.
int draw_menu_with_imgui(const std::vector<MenuItem> &items);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_MENU_H
