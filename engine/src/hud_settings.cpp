// Reads and writes hud.ini: see hud_settings.h.

#include "hud_settings.h"

#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>

#include "trace_folders.h"

namespace sessionscope {

namespace {

std::optional<int> to_int(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// "x,y,width,height", for example "120,80,1920,1240". A window smaller than 50 units is not one.
std::optional<WindowPlace> to_place(std::string_view text) {
    int numbers[4];
    for (int i = 0; i < 4; ++i) {
        const std::size_t comma = text.find(',');
        const auto number = to_int(text.substr(0, comma));
        if (!number || (i < 3 && comma == std::string_view::npos)) return std::nullopt;
        numbers[i] = *number;
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    if (!text.empty() || numbers[2] < 50 || numbers[3] < 50) return std::nullopt;
    return WindowPlace{numbers[0], numbers[1], numbers[2], numbers[3]};
}

std::string place_text(const WindowPlace &p) {
    return std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.width) + "," +
           std::to_string(p.height);
}

// "meter,cpu,cores": the ids in order. Empty names are skipped.
std::vector<std::string> to_list(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        if (!item.empty()) out.emplace_back(item);
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return out;
}

std::optional<bool> to_bool(std::string_view text) {
    if (text == "yes") return true;
    if (text == "no") return false;
    return std::nullopt;
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
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string_view key(line.data(), equals);
        const std::string_view value(line.data() + equals + 1, line.size() - equals - 1);

        if (key == "window") settings.window = to_place(value);
        else if (key == "on_top" && to_bool(value)) settings.on_top = *to_bool(value);
        else if (key == "modules") settings.modules = to_list(value);   // empty: none shown
        else if (key == "vertical" && to_bool(value)) settings.vertical = *to_bool(value);
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
    return static_cast<bool>(out);
}

}  // namespace sessionscope
