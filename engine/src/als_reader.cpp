// als_reader.cpp - see als_reader.h: the plug-ins in an Ableton Live set, with their saved state.

#include "als_reader.h"

#include "inflate.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace sessionscope {

namespace {

// One XML tag: its name, the value of its Value="..." attribute, and what kind of tag it is.
struct Tag {
    std::string name, value;
    bool closing = false;       // </Name>
    bool self_closing = false;  // <Name ... />
};

// Reads tags one after another. Live's XML has no comments or CDATA, so this is enough; text
// between tags (the hex states) is handed out separately.
struct Scanner {
    const std::string &xml;
    std::size_t at = 0;

    // The next tag, and the text before it. False at the end.
    bool next(Tag &tag, std::string &text_before) {
        const std::size_t open = xml.find('<', at);
        if (open == std::string::npos) return false;
        text_before.assign(xml, at, open - at);
        const std::size_t close = xml.find('>', open);
        if (close == std::string::npos) return false;
        at = close + 1;

        const std::string inside = xml.substr(open + 1, close - open - 1);
        tag = Tag{};
        if (!inside.empty() && (inside[0] == '?' || inside[0] == '!')) {   // <?xml ...?>: not an element
            tag.self_closing = true;
            return true;
        }
        tag.closing = !inside.empty() && inside[0] == '/';
        tag.self_closing = !inside.empty() && inside.back() == '/';
        const std::size_t name_start = tag.closing ? 1 : 0;
        const std::size_t name_end = inside.find_first_of(" \t\r\n/", name_start);
        tag.name = inside.substr(name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start);
        const std::size_t value = inside.find(" Value=\"");
        if (value != std::string::npos) {
            const std::size_t start = value + 8, end = inside.find('"', start);
            tag.value = unescape(inside.substr(start, end - start));
        }
        return true;
    }

    static std::string unescape(const std::string &text) {   // the five XML entities
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '&') {
                out += text[i];
                continue;
            }
            const std::size_t semi = text.find(';', i);
            const std::string entity = text.substr(i + 1, semi - i - 1);
            out += entity == "amp" ? "&" : entity == "lt" ? "<" : entity == "gt" ? ">" : entity == "quot" ? "\"" : entity == "apos" ? "'" : "?";
            i = semi == std::string::npos ? text.size() : semi;
        }
        return out;
    }
};

// Hex text (whitespace ignored) to bytes. False if a character is not hex or one is left over.
bool hex_bytes(const std::string &hex, std::string &out) {
    out.clear();
    int high = -1;
    for (const char c : hex) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) return false;
        if (high < 0) {
            high = digit;
        } else {
            out += static_cast<char>(high * 16 + digit);
            high = -1;
        }
    }
    return high < 0;
}

bool is_track(const std::string &name) {
    return name == "MidiTrack" || name == "AudioTrack" || name == "ReturnTrack" || name == "GroupTrack" ||
           name == "MainTrack" || name == "MasterTrack";
}

}  // namespace

std::vector<ProjectPlugin> parse_als_plugins(const std::string &xml) {
    std::vector<ProjectPlugin> plugins;
    std::vector<std::string> open;     // the open elements, outermost first
    std::string track;
    int track_number = 0, tracks_seen = 0;
    std::size_t track_depth = 0;       // where the current track element sits in `open` (0: none)
    std::size_t device_depth = 0;      // the same for the current <PluginDevice>
    std::int64_t uid[4] = {0, 0, 0, 0};
    std::string state_text;            // hex inside <ProcessorState> or <ControllerState>

    Scanner scanner{xml};
    Tag tag;
    std::string text;
    while (scanner.next(tag, text)) {
        const std::string parent = open.empty() ? "" : open.back();
        if (tag.closing) {
            if (!open.empty()) open.pop_back();
            ProjectPlugin *plugin = device_depth && !plugins.empty() ? &plugins.back() : nullptr;
            if (plugin && (tag.name == "ProcessorState" || tag.name == "ControllerState")) {
                std::string bytes;
                if (!hex_bytes(text, bytes)) plugin->problem = "the saved state is not hex";
                (tag.name == "ProcessorState" ? plugin->component : plugin->controller) = bytes;
            }
            if (tag.name == "PluginDevice") device_depth = 0;
            if (is_track(tag.name) && open.size() + 1 == track_depth) {
                track_depth = 0;
                track.clear();
            }
            continue;
        }
        if (tag.name.empty() || tag.name[0] == '?' || tag.name[0] == '!') continue;

        // Opening tags (and self-closing ones, which close at once).
        if (is_track(tag.name)) {
            const bool main = tag.name == "MainTrack" || tag.name == "MasterTrack";
            track_number = main ? 0 : ++tracks_seen;
            track = main ? "MASTER" : "";
            track_depth = open.size() + 1;
        } else if (tag.name == "EffectiveName" && parent == "Name" && track_depth && track.empty() &&
                   open.size() == track_depth + 1) {
            track = tag.value;   // the track's own name: <Track><Name><EffectiveName>
        } else if (tag.name == "PluginDevice") {
            ProjectPlugin plugin;
            plugin.track = track;
            plugin.track_number = track_number;
            plugins.push_back(plugin);
            device_depth = open.size() + 1;
        } else if (device_depth && !plugins.empty()) {
            ProjectPlugin &plugin = plugins.back();
            // The device's own on/off switch: <PluginDevice><On><Manual Value="true"/>.
            if (tag.name == "Manual" && parent == "On" && open.size() == device_depth + 1) plugin.bypassed = tag.value == "false";
            if (tag.name == "Vst3PluginInfo") plugin.kind = "VST3";
            if (tag.name == "VstPluginInfo") plugin.kind = "VST";
            if (tag.name == "AuPluginInfo") plugin.kind = "AU";
            if (tag.name == "Name" && (parent == "Vst3PluginInfo" || parent == "AuPluginInfo")) plugin.name = tag.value;
            if (tag.name == "PlugName" && parent == "VstPluginInfo") plugin.name = tag.value;
            if (tag.name.rfind("Fields.", 0) == 0 && parent == "Uid" && open.size() >= 2 &&
                open[open.size() - 2] == "Vst3PluginInfo") {
                const int field = tag.name.back() - '0';
                if (field >= 0 && field < 4) uid[field] = std::stoll(tag.value.empty() ? "0" : tag.value);
                if (field == 3) {
                    char hex[33];
                    std::snprintf(hex, sizeof hex, "%08X%08X%08X%08X", static_cast<std::uint32_t>(uid[0]), static_cast<std::uint32_t>(uid[1]),
                                  static_cast<std::uint32_t>(uid[2]), static_cast<std::uint32_t>(uid[3]));
                    plugin.class_id = hex;
                }
            }
        }
        if (!tag.self_closing) open.push_back(tag.name);
    }
    for (ProjectPlugin &plugin : plugins) {
        if (plugin.kind.empty()) plugin.kind = "unknown";
        if (plugin.kind == "VST3" && plugin.component.empty() && plugin.problem.empty()) plugin.problem = "no saved state in the set";
    }
    return plugins;
}

std::vector<ProjectPlugin> read_als_plugins(const std::filesystem::path &set) {
    std::ifstream file(set, std::ios::binary);
    if (!file) throw std::runtime_error("Can't open " + set.u8string());
    const std::string packed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_als_plugins(gunzip(packed));
}

}  // namespace sessionscope
