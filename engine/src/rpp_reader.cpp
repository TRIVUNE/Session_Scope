// rpp_reader.cpp - see rpp_reader.h: the plug-ins in a REAPER project, with their saved state.

#include "rpp_reader.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace sessionscope {

namespace {

// One line of an .rpp split into words. A word may be quoted with ", ' or ` (REAPER picks a quote
// character the text itself doesn't contain); the quotes are removed.
std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        const char quote = line[i];
        if (quote == '"' || quote == '\'' || quote == '`') {
            const std::size_t end = line.find(quote, i + 1);
            out.push_back(line.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
        } else {
            std::size_t end = i;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
            out.push_back(line.substr(i, end - i));
            i = end;
        }
    }
    return out;
}

std::string trimmed(const std::string &line) {
    const std::size_t start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    const std::size_t end = line.find_last_not_of(" \t\r");
    return line.substr(start, end - start + 1);
}

// Reads little-endian numbers out of the decoded bytes, keeping track of where it is.
struct Reader {
    const std::string &bytes;
    std::size_t at = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (at + 4 > bytes.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t value = 0;
        for (int b = 3; b >= 0; --b) value = (value << 8) | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(b)]);
        at += 4;
        return value;
    }
    std::string take(std::size_t count) {
        if (at + count > bytes.size()) {
            ok = false;
            return {};
        }
        std::string out = bytes.substr(at, count);
        at += count;
        return out;
    }
    void skip(std::size_t count) {
        if (at + count > bytes.size()) ok = false;
        else at += count;
    }
};

// Parts 1 and 2 of a VST3 block (see rpp_reader.h): fills component and controller, or problem.
void read_vst3_state(const std::string &bytes, RppPlugin &plugin) {
    Reader r{bytes};
    r.u32();                         // REAPER's id for the plug-in
    r.u32();                         // a fixed magic number
    r.skip(8u * r.u32());            // input pins: a 64-bit channel mask each
    r.skip(8u * r.u32());            // output pins
    const std::uint32_t state_size = r.u32();
    r.u32();                         // 1
    r.u32();                         // flags
    const std::size_t state_start = r.at;
    const std::uint32_t component_size = r.u32();
    r.u32();                         // 1
    plugin.component = r.take(component_size);
    const std::uint32_t controller_size = r.u32();
    r.u32();                         // 1
    plugin.controller = r.take(controller_size);
    // The sizes must add up exactly: a layout we don't know must never feed a plug-in garbage.
    if (!r.ok || r.at - state_start != state_size) {
        plugin.component.clear();
        plugin.controller.clear();
        plugin.problem = "the saved state is not in the layout this reader knows";
    }
}

}  // namespace

std::string decode_base64(const std::string &text) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        const std::size_t value = alphabet.find(c);
        if (value == std::string::npos) break;   // "=" padding, or anything else: the end
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

std::vector<RppPlugin> parse_rpp_plugins(const std::string &text) {
    std::vector<RppPlugin> plugins;
    std::istringstream lines(text);
    std::string raw;
    std::vector<std::string> blocks;   // the open blocks, outermost first: "REAPER_PROJECT", "TRACK", "FXCHAIN"...
    std::string track;                 // the track we are in
    int track_number = 0, tracks_seen = 0;
    bool bypassed = false, offline = false;   // from the last BYPASS line
    RppPlugin *current = nullptr;             // the plug-in block we are inside
    std::string base64;                       // its decoded lines, joined

    while (std::getline(lines, raw)) {
        const std::string line = trimmed(raw);
        if (line.empty()) continue;

        if (current) {   // inside a plug-in block: base64 lines until its ">"
            if (line == ">") {
                blocks.pop_back();
                if (current->kind == "VST3") read_vst3_state(base64, *current);
                current = nullptr;
                base64.clear();
            } else {
                base64 += decode_base64(line);   // each line is complete on its own
            }
            continue;
        }

        if (line[0] == '<') {
            const std::vector<std::string> w = words(line.substr(1));
            const std::string block = w.empty() ? "" : w[0];
            blocks.push_back(block);
            if (block == "TRACK") {
                ++tracks_seen;
                track_number = tracks_seen;
                track.clear();
            } else if (block == "MASTERFXLIST") {
                track_number = 0;
                track = "MASTER";
            } else if (block == "VST" || block == "JS" || block == "CLAP" || block == "AU" || block == "DX" ||
                       block == "LV2") {
                RppPlugin plugin;
                plugin.track = track;
                plugin.track_number = track_number;
                plugin.name = w.size() > 1 ? w[1] : "";
                plugin.file = w.size() > 2 ? w[2] : "";
                plugin.kind = block;
                if (block == "VST") {   // "VST3: ..." / "VST3i: ..." are VST3; "VST: ..." / "VSTi: ..." are VST2
                    plugin.kind = plugin.name.rfind("VST3", 0) == 0 ? "VST3" : "VST";
                    // The id comes as "number{32 hex digits}" in the 6th word:
                    // <VST "name" file 0 "" number{id} "">
                    const std::string id = w.size() > 5 ? w[5] : "";
                    const std::size_t open = id.find('{'), close = id.find('}');
                    if (plugin.kind == "VST3" && open != std::string::npos && close == open + 33)
                        plugin.class_id = id.substr(open + 1, 32);
                }
                plugin.bypassed = bypassed;
                plugin.offline = offline;
                bypassed = offline = false;
                plugins.push_back(plugin);
                current = &plugins.back();
            }
            continue;
        }
        if (line == ">") {
            if (!blocks.empty()) {
                if (blocks.back() == "TRACK" || blocks.back() == "MASTERFXLIST") track.clear();
                blocks.pop_back();
            }
            continue;
        }

        const std::vector<std::string> w = words(line);
        if (w.empty()) continue;
        // The track's own NAME line (directly inside <TRACK, not inside one of its items).
        if (w[0] == "NAME" && !blocks.empty() && blocks.back() == "TRACK" && w.size() > 1) track = w[1];
        if (w[0] == "BYPASS" && w.size() > 2) {
            bypassed = w[1] == "1";
            offline = w[2] == "1";
        }
    }
    return plugins;
}

std::vector<RppPlugin> read_rpp_plugins(const std::filesystem::path &project) {
    std::ifstream file(project, std::ios::binary);
    if (!file) throw std::runtime_error("Can't open " + project.u8string());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_rpp_plugins(text);
}

}  // namespace sessionscope
