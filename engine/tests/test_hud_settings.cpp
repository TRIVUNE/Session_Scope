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
        CHECK(!s.full && !s.compact);
        CHECK(!s.compact_mode && s.on_top && s.list_open && s.sort_key == "bar");
    }

    // What is saved comes back.
    {
        HudSettings saved;
        saved.full = WindowPlace{-1200, 40, 1920, 1240};   // a screen left of the main one: negative x
        saved.compact = WindowPlace{300, 900, 1120, 300};
        saved.compact_mode = true;
        saved.on_top = false;
        saved.list_open = false;
        saved.sort_key = "worst";
        CHECK(save_hud_settings(file, saved));

        const HudSettings s = load_hud_settings(file);
        CHECK(s.full && s.full->x == -1200 && s.full->y == 40 && s.full->width == 1920 && s.full->height == 1240);
        CHECK(s.compact && s.compact->x == 300 && s.compact->height == 300);
        CHECK(s.compact_mode && !s.on_top && !s.list_open && s.sort_key == "worst");
    }

    // A damaged or hand-edited file: bad lines are skipped, good ones still count.
    {
        std::ofstream(file) << "window=12,34\r\n"            // too few numbers
                            << "compact_window=1,2,3,4\n"   // too small to be a window
                            << "on_top=maybe\n"             // not yes or no
                            << "list=no\r\n"                // fine, with a Windows line ending
                            << "no equals sign here\n"
                            << "unknown=1\n";
        const HudSettings s = load_hud_settings(file);
        CHECK(!s.full && !s.compact);
        CHECK(s.on_top);        // the bad value left the default
        CHECK(!s.list_open);    // the good line counted
    }

    // No settings folder at all: saving says so instead of crashing.
    CHECK(!save_hud_settings({}, HudSettings{}));

    std::filesystem::remove_all(folder);
    return finish_tests();
}
