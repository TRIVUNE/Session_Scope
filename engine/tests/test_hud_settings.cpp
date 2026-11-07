// Tests for hud.ini: what is saved comes back, and a damaged or hand-edited file never breaks the HUD.

#include "hud_settings.h"
#include "test_support.h"

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
        CHECK((s.modules == std::vector<std::string>{"meter", "cpu", "cores", "bars"}));
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
