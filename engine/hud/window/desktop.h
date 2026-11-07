#ifndef SESSIONSCOPE_HUD_DESKTOP_H
#define SESSIONSCOPE_HUD_DESKTOP_H

// =================================================================================================
// desktop.h - the HUD and the rest of the desktop: tray icon, hotkey, taskbar button
// =================================================================================================
//
//   tray icon   a small icon in the notification area (bottom right, next to the clock). Click it
//               to show or hide the HUD; right-click for a menu with "Quit".
//   hotkey      a key combination (Ctrl+Alt+H unless changed in hud.ini) that shows or hides the
//               HUD from any program, even while the DAW has the keyboard.
//   taskbar     whether the HUD has a button in the taskbar. With several pop-out windows that's a
//               lot of buttons; without them, the tray icon is how to reach the HUD.
//
// Where it comes from: MiniMeters has all three (seen with REA: "MiniMeters Tray Icon",
// RegisterHotKey, "HIDE_FROM_TASKBAR"). The Windows functions are Shell_NotifyIconW, RegisterHotKey
// and the WS_EX_TOOLWINDOW window style, documented by Microsoft under those names.
//
// Windows only for now. On macOS every function here does nothing and available() is false, so
// the menu leaves these items out.

#include <memory>
#include <string>

struct GLFWwindow;

namespace sessionscope {

class Desktop {
public:
    Desktop();
    ~Desktop();
    Desktop(const Desktop &) = delete;
    Desktop &operator=(const Desktop &) = delete;

    // True where the tray icon and hotkey exist (Windows).
    static bool available();

    // Shows or removes the tray icon.
    void set_tray_icon(bool on);
    bool tray_icon() const { return tray_on_; }

    // Registers a hotkey from text like "ctrl+alt+h", "ctrl+shift+f12" or "win+alt+s". Returns
    // false if the text isn't a key combination or another program already uses it.
    bool set_hotkey(const std::string &text);
    // "Ctrl+Alt+H" for the menu, or why it isn't working.
    const std::string &hotkey_label() const { return hotkey_label_; }

    // What happened since the last call (clicks on the tray icon, the hotkey, its menu).
    struct Events {
        bool toggle = false;   // show the HUD if hidden, hide it if shown
        bool quit = false;     // "Quit" in the tray icon's menu
    };
    Events take_events();

    // Shows or removes a window's button in the taskbar (and in Alt+Tab).
    static void set_in_taskbar(GLFWwindow *window, bool in_taskbar);

private:
    struct Impl;                    // the Windows parts, in desktop.cpp
    std::unique_ptr<Impl> impl_;
    bool tray_on_ = false;
    std::string hotkey_label_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_DESKTOP_H
