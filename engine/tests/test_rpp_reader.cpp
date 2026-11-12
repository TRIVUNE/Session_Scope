// Tests for the REAPER project reader (rpp_reader.h): track names, bypass, plug-in kinds, and the
// VST3 state taken apart into its two halves. The project text is built here the way REAPER 7.83
// writes it (docs/research/rpp-state.md), with states of known bytes.

#include "rpp_reader.h"
#include "test_support.h"

#include <cstdint>
#include <string>

using sessionscope::decode_base64;
using sessionscope::parse_rpp_plugins;
using sessionscope::ProjectPlugin;

namespace {

std::string encode_base64(const std::string &bytes) {
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) chunk |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) chunk |= static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

std::string u32(std::uint32_t value) {
    std::string out;
    for (int b = 0; b < 4; ++b) out += static_cast<char>((value >> (8 * b)) & 0xFF);
    return out;
}

// One part, as REAPER writes it: base64 in lines of at most 128 characters.
std::string lines_of(const std::string &bytes) {
    const std::string text = encode_base64(bytes);
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 128) out += "      " + text.substr(i, 128) + "\n";
    return out;
}

// A VST3 block: REAPER's header (2 inputs, 2 outputs), the state, the preset name.
std::string vst3_block(const std::string &name, const std::string &file, const std::string &component,
                       const std::string &controller, bool break_sizes = false) {
    const std::string state = u32(static_cast<std::uint32_t>(component.size())) + u32(1) + component +
                              u32(static_cast<std::uint32_t>(controller.size())) + u32(1) + controller;
    std::string header = u32(1234) + u32(0xfeed5eee) + u32(2) + std::string(16, '\1') + u32(2) + std::string(16, '\2');
    header += u32(static_cast<std::uint32_t>(state.size() + (break_sizes ? 1 : 0))) + u32(1) + u32(0xffff);
    return "    <VST \"" + name + "\" " + file + " 0 \"\" 1234{5653544B436C336B636C697033000000} \"\"\n" + lines_of(header) +
           lines_of(state) + lines_of(std::string(6, '\0')) + "    >\n";
}

}  // namespace

int main() {
    CHECK(decode_base64("SGVsbG8=") == "Hello");
    CHECK(decode_base64("SGVs bG8=") == "Hello");   // spaces are ignored

    // A component state longer than one 128-character line, so it spans several lines.
    const std::string long_component(500, 'C');
    const std::string project =
        "<REAPER_PROJECT 0.1 \"7.83/win64\" 1\n"
        "  <MASTERFXLIST\n"
        "    BYPASS 0 0 0\n" +
        vst3_block("VST3: Limiter (Someone)", "Limiter.vst3", "master", "") +
        "  >\n"
        "  <TRACK {AAAA}\n"
        "    NAME \"Lead synth\"\n"
        "    <ITEM\n"
        "      NAME \"not the track\"\n"
        "    >\n"
        "    <FXCHAIN\n"
        "      BYPASS 0 0 0\n" +
        vst3_block("VST3i: Synth (Vendor)", "\"My Synth.vst3\"", long_component, "controller bytes") +
        "      BYPASS 1 0 0\n" +
        vst3_block("VST3: Reverb (Vendor)", "Reverb.vst3", "reverb", "") +
        "      BYPASS 0 0 0\n"
        "      <VST \"VST: Old Delay (Vendor)\" OldDelay.dll 0 \"\" 1145392228<5653544F> \"\"\n"
        "        AAAA\n"
        "      >\n"
        "      BYPASS 0 1 0\n" +
        vst3_block("VST3: Broken (Vendor)", "Broken.vst3", "data", "", true) +
        "    >\n"
        "  >\n"
        "  <TRACK {BBBB}\n"
        "    <FXCHAIN\n"
        "      BYPASS 0 0 0\n"
        "      <JS \"utility/volume\" \"\"\n"
        "        0 - - -\n"
        "      >\n"
        "    >\n"
        "  >\n"
        ">\n";

    const std::vector<ProjectPlugin> plugins = parse_rpp_plugins(project);
    CHECK(plugins.size() == 6);
    if (plugins.size() == 6) {
        const ProjectPlugin &master = plugins[0];
        CHECK(master.track == "MASTER" && master.track_number == 0);
        CHECK(master.kind == "VST3" && master.component == "master" && master.controller.empty());

        const ProjectPlugin &synth = plugins[1];
        CHECK(synth.track == "Lead synth");   // the track's own NAME, not the item's
        CHECK(synth.track_number == 1);
        CHECK(synth.file == "My Synth.vst3");
        CHECK(synth.class_id == "5653544B436C336B636C697033000000");
        CHECK(synth.component == long_component);
        CHECK(synth.controller == "controller bytes");
        CHECK(synth.problem.empty() && !synth.bypassed);

        CHECK(plugins[2].bypassed && plugins[2].component == "reverb");
        CHECK(plugins[3].kind == "VST" && plugins[3].component.empty());   // VST2: no VST3 state is read

        const ProjectPlugin &broken = plugins[4];
        CHECK(broken.offline);
        CHECK(!broken.problem.empty() && broken.component.empty());   // sizes that don't add up: no state at all

        CHECK(plugins[5].kind == "JS" && plugins[5].track == "" && plugins[5].track_number == 2);
    }
    return finish_tests();
}
