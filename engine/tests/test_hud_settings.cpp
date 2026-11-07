// Tests for hud.ini: what is saved comes back, and a damaged or hand-edited file never breaks the HUD.

#include "hud_settings.h"
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace sessionscope;

int main() {
    const auto folder = std::filesystem::temp_directory_path() / "sessionscope_test_hud_settings";
    std::filesystem::remove_all(folder);
    const auto file = folder / "hud.ini";   // the folder doesn't exist yet: saving creates it

    // Nothing saved yet: the defaults.
    {
        const HudSettings s = load_hud_settings(file);
        CHECK(!s.window);
        CHECK(s.on_top && !s.vertical);
        CHECK((s.modules == std::vector<std::string>{"meter", "cpu", "cores", "tasks", "busiest", "dropouts", "memory", "clock"}));
    }

    // What is saved comes back.
    {
        HudSettings saved;
        saved.window = WindowPlace{-1200, 40, 1920, 160};   // a screen left of the main one: negative x
        saved.on_top = false;
        saved.modules = {"cores", "meter"};
        saved.vertical = true;
        CHECK(save_hud_settings(file, saved));

        const HudSettings s = load_hud_settings(file);
        CHECK(s.window && s.window->x == -1200 && s.window->y == 40 && s.window->width == 1920 && s.window->height == 160);
        CHECK(!s.on_top && s.vertical);
        CHECK((s.modules == std::vector<std::string>{"cores", "meter"}));
    }

    // The newer settings: sizes, frame rate, opacity, tray, taskbar, hotkey, pop-outs.
    {
        HudSettings saved;
        saved.sizes_across = {{"cores", 640}, {"busiest", 160}};
        saved.sizes_down = {{"cores", 300}};
        saved.fps = 60;
        saved.opacity = 75;
        saved.tray = true;
        saved.taskbar = false;
        saved.hotkey = "ctrl+shift+f12";
        saved.popouts = {{"dropouts", WindowPlace{1500, 80, 180, 160}}};
        CHECK(save_hud_settings(file, saved));

        const HudSettings s = load_hud_settings(file);
        CHECK((s.sizes_across == std::vector<ModuleSize>{{"cores", 640}, {"busiest", 160}}));
        CHECK((s.sizes_down == std::vector<ModuleSize>{{"cores", 300}}));
        CHECK(s.fps == 60 && s.opacity == 75 && s.tray && !s.taskbar && s.hotkey == "ctrl+shift+f12");
        CHECK(s.popouts.size() == 1 && s.popouts[0].module == "dropouts" && s.popouts[0].place.x == 1500 &&
              s.popouts[0].place.height == 160);
        // A popped-out module is not also in the strip.
        CHECK(std::find(s.modules.begin(), s.modules.end(), "dropouts") == s.modules.end());
    }

    // Bad values in the newer settings fall back to the defaults.
    {
        std::ofstream(file) << "fps=45\n"                          // only 0, 30 or 60
                            << "opacity=5\n"                       // clamped up to 25
                            << "sizes_across=cores:x,:5,meter:30\n"   // only the valid piece stays
                            << "popout=cores@1,2\n";               // not a window place
        const HudSettings s = load_hud_settings(file);
        CHECK(s.fps == 0);
        CHECK(s.opacity == 25);
        CHECK((s.sizes_across == std::vector<ModuleSize>{{"meter", 30}}));
        CHECK(s.popouts.empty());
    }

    // Every module hidden is a choice too, and comes back as such.
    {
        HudSettings saved;
        saved.modules.clear();
        CHECK(save_hud_settings(file, saved));
        CHECK(load_hud_settings(file).modules.empty());
    }

    // A damaged, hand-edited or older file: bad lines are skipped, good ones still count.
    {
        std::ofstream(file) << "window=12,34\r\n"            // too few numbers
                            << "compact_window=1,2,3,4\n"   // an older HUD's key: ignored
                            << "on_top=maybe\n"             // not yes or no
                            << "vertical=yes\r\n"           // fine, with a Windows line ending
                            << "modules=cpu,,cores\n"       // an empty name is skipped
                            << "no equals sign here\n"
                            << "unknown=1\n";
        const HudSettings s = load_hud_settings(file);
        CHECK(!s.window);
        CHECK(s.on_top);      // the bad value left the default
        CHECK(s.vertical);    // the good line counted
        CHECK((s.modules == std::vector<std::string>{"cpu", "cores"}));
    }

    // No settings folder at all: saving says so instead of crashing.
    CHECK(!save_hud_settings({}, HudSettings{}));

    std::filesystem::remove_all(folder);
    return finish_tests();
}
