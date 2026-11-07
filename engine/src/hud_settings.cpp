// =================================================================================================
// hud_settings.cpp - reads and writes hud.ini (see hud_settings.h)
// =================================================================================================

#include "hud_settings.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>

#include "trace_folders.h"

namespace sessionscope {

namespace {

// Each to_...() turns the text after "=" into a value, or nothing if the text is not valid.

// "42" -> 42. Anything else (letters, "4x", empty) -> nothing.
std::optional<int> to_int(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// Splits "a,b,,c" at a separator into {"a", "b", "c"} (empty pieces are dropped).
std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t at = text.find(separator);
        const std::string_view piece = text.substr(0, at);
        if (!piece.empty()) out.push_back(piece);
        text = at == std::string_view::npos ? std::string_view() : text.substr(at + 1);
    }
    return out;
}

// "x,y,width,height", for example "120,80,1920,1240". A window smaller than 50 units is not one.
std::optional<WindowPlace> to_place(std::string_view text) {
    const std::vector<std::string_view> parts = split(text, ',');
    if (parts.size() != 4) return std::nullopt;
    int n[4];
    for (int i = 0; i < 4; ++i) {
        const auto number = to_int(parts[static_cast<std::size_t>(i)]);
        if (!number) return std::nullopt;
        n[i] = *number;
    }
    if (n[2] < 50 || n[3] < 50) return std::nullopt;
    return WindowPlace{n[0], n[1], n[2], n[3]};
}

std::string place_text(const WindowPlace &p) {
    return std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.width) + "," + std::to_string(p.height);
}

std::optional<bool> to_bool(std::string_view text) {
    if (text == "yes") return true;
    if (text == "no") return false;
    return std::nullopt;
}

// "cores:640,busiest:160" -> {("cores", 640), ("busiest", 160)}. Bad pieces are skipped.
std::vector<ModuleSize> to_sizes(std::string_view text) {
    std::vector<ModuleSize> out;
    for (const std::string_view piece : split(text, ',')) {
        const std::size_t colon = piece.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        const auto length = to_int(piece.substr(colon + 1));
        if (length && *length > 0) out.emplace_back(std::string(piece.substr(0, colon)), *length);
    }
    return out;
}

std::string sizes_text(const std::vector<ModuleSize> &sizes) {
    std::string out;
    for (const ModuleSize &size : sizes) out += (out.empty() ? "" : ",") + size.first + ":" + std::to_string(size.second);
    return out;
}

// "dropouts@1500,80,180,160" -> a pop-out.
std::optional<PopOut> to_popout(std::string_view text) {
    const std::size_t at = text.find('@');
    if (at == std::string_view::npos || at == 0) return std::nullopt;
    const auto place = to_place(text.substr(at + 1));
    if (!place) return std::nullopt;
    return PopOut{std::string(text.substr(0, at)), *place};
}

}  // namespace

std::filesystem::path hud_settings_file() {
    const std::filesystem::path folder = settings_folder();
    return folder.empty() ? folder : folder / "hud.ini";
}

HudSettings load_hud_settings(const std::filesystem::path &file) {
    HudSettings settings;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // a file saved with Windows line endings
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;   // comments, junk
        const std::string_view key(line.data(), equals);
        const std::string_view value(line.data() + equals + 1, line.size() - equals - 1);

        if (key == "window") settings.window = to_place(value);
        else if (key == "on_top" && to_bool(value)) settings.on_top = *to_bool(value);
        else if (key == "modules") {   // empty is a valid choice: no modules in the strip
            settings.modules.clear();
            for (const std::string_view id : split(value, ',')) settings.modules.emplace_back(id);
        }
        else if (key == "vertical" && to_bool(value)) settings.vertical = *to_bool(value);
        else if (key == "sizes_across") settings.sizes_across = to_sizes(value);
        else if (key == "sizes_down") settings.sizes_down = to_sizes(value);
        else if (key == "fps" && to_int(value) && (*to_int(value) == 0 || *to_int(value) == 30 || *to_int(value) == 60)) {
            settings.fps = *to_int(value);
        }
        else if (key == "opacity" && to_int(value)) settings.opacity = std::clamp(*to_int(value), 25, 100);
        else if (key == "tray" && to_bool(value)) settings.tray = *to_bool(value);
        else if (key == "taskbar" && to_bool(value)) settings.taskbar = *to_bool(value);
        else if (key == "hotkey" && !value.empty()) settings.hotkey = std::string(value);
        else if (key == "popout") {
            if (const auto popout = to_popout(value)) settings.popouts.push_back(*popout);
        }
    }
    // A module can't be both in the strip and popped out: the pop-out wins.
    for (const PopOut &popout : settings.popouts) {
        settings.modules.erase(std::remove(settings.modules.begin(), settings.modules.end(), popout.module),
                               settings.modules.end());
    }
    return settings;
}

bool save_hud_settings(const std::filesystem::path &file, const HudSettings &settings) {
    if (file.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file);
    if (!out) return false;
    const auto yes_no = [](bool b) { return b ? "yes" : "no"; };
    out << "# SessionScope HUD settings, written when the HUD closes. Delete this file to start fresh.\n";
    if (settings.window) out << "window=" << place_text(*settings.window) << "\n";
    out << "on_top=" << yes_no(settings.on_top) << "\n";
    out << "modules=";
    for (std::size_t i = 0; i < settings.modules.size(); ++i) out << (i ? "," : "") << settings.modules[i];
    out << "\n";
    out << "vertical=" << yes_no(settings.vertical) << "\n";
    out << "sizes_across=" << sizes_text(settings.sizes_across) << "\n";
    out << "sizes_down=" << sizes_text(settings.sizes_down) << "\n";
    out << "fps=" << settings.fps << "\n";
    out << "opacity=" << settings.opacity << "\n";
    out << "tray=" << yes_no(settings.tray) << "\n";
    out << "taskbar=" << yes_no(settings.taskbar) << "\n";
    out << "hotkey=" << settings.hotkey << "\n";
    for (const PopOut &popout : settings.popouts) out << "popout=" << popout.module << "@" << place_text(popout.place) << "\n";
    return static_cast<bool>(out);
}

}  // namespace sessionscope
