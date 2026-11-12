// Tests for the Ableton Live set reader (als_reader.h), on XML shaped like what Live 12 saved on
// 12 Nov 2026 (docs/research/als-state.md): track names, the on/off switch, the id from the four
// Uid fields (negative numbers included), the two state halves from hex, devices in a rack, and
// VST2 plug-ins listed without state.

#include "als_reader.h"
#include "test_support.h"

#include <string>

using sessionscope::parse_als_plugins;
using sessionscope::ProjectPlugin;

namespace {

std::string vst3_device(const std::string &name, bool on, const std::string &fields, const std::string &processor,
                        const std::string &controller) {
    return "<PluginDevice Id=\"0\">\n"
           "  <On>\n    <LomId Value=\"0\" />\n    <Manual Value=\"" + std::string(on ? "true" : "false") + "\" />\n  </On>\n"
           "  <PluginDesc>\n    <Vst3PluginInfo Id=\"0\">\n      <Preset>\n        <Vst3Preset Id=\"2\">\n"
           "          <Uid>" + fields + "</Uid>\n"
           "          <ProcessorState>\n            " + processor + "\n          </ProcessorState>\n" +
           (controller.empty() ? "          <ControllerState />\n" : "          <ControllerState>" + controller + "</ControllerState>\n") +
           "          <Name Value=\"a preset name\" />\n        </Vst3Preset>\n      </Preset>\n"
           "      <Name Value=\"" + name + "\" />\n"
           "      <Uid>" + fields + "</Uid>\n"
           "    </Vst3PluginInfo>\n  </PluginDesc>\n"
           "  <ParameterList><PluginFloatParameter><Name Value=\"not the plug-in\" /></PluginFloatParameter></ParameterList>\n"
           "</PluginDevice>\n";
}

std::string uid(long long a, long long b, long long c, long long d) {
    return "<Fields.0 Value=\"" + std::to_string(a) + "\" /><Fields.1 Value=\"" + std::to_string(b) + "\" /><Fields.2 Value=\"" +
           std::to_string(c) + "\" /><Fields.3 Value=\"" + std::to_string(d) + "\" />";
}

}  // namespace

int main() {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Ableton>\n<LiveSet>\n<Tracks>\n"
        "<MidiTrack Id=\"12\">\n  <Name>\n    <EffectiveName Value=\"1-Vital\" />\n    <UserName Value=\"\" />\n  </Name>\n"
        "  <DeviceChain><Devices>\n" +
        vst3_device("Vital", true, uid(1448301654, 1769234806, 1769234796, 0), "56 73 74 57\n            00 01", "") +
        "  </Devices></DeviceChain>\n</MidiTrack>\n"
        "<AudioTrack Id=\"8\">\n  <Name>\n    <EffectiveName Value=\"Bass &amp; Drums\" />\n  </Name>\n"
        "  <DeviceChain><Devices>\n"
        "    <AudioEffectGroupDevice Id=\"1\"><Branches><AudioEffectBranch><DeviceChain><Devices>\n" +
        vst3_device("Pro-Q 4", false, uid(-312951438, 1549813326, -1505897740, 7638895), "AABB", "CCDD") +
        "    </Devices></DeviceChain></AudioEffectBranch></Branches></AudioEffectGroupDevice>\n"
        "    <PluginDevice Id=\"3\"><On><Manual Value=\"true\" /></On><PluginDesc><VstPluginInfo Id=\"0\">"
        "<PlugName Value=\"OldDelay\" /></VstPluginInfo></PluginDesc></PluginDevice>\n"
        "  </Devices></DeviceChain>\n</AudioTrack>\n"
        "</Tracks>\n<MainTrack>\n  <Name>\n    <EffectiveName Value=\"Main\" />\n  </Name>\n</MainTrack>\n"
        "</LiveSet>\n</Ableton>\n";

    const auto plugins = parse_als_plugins(xml);
    CHECK(plugins.size() == 3);
    if (plugins.size() == 3) {
        const ProjectPlugin &vital = plugins[0];
        CHECK(vital.track == "1-Vital" && vital.track_number == 1);
        CHECK(vital.kind == "VST3" && vital.name == "Vital" && !vital.bypassed);
        CHECK(vital.class_id == "56535456697461766974616C00000000");
        CHECK(vital.component == std::string("\x56\x73\x74\x57\x00\x01", 6));   // hex across lines, spaces ignored
        CHECK(vital.controller.empty() && vital.problem.empty() && vital.file.empty());

        const ProjectPlugin &eq = plugins[1];   // inside a rack, switched off
        CHECK(eq.track == "Bass & Drums" && eq.track_number == 2);
        CHECK(eq.bypassed);
        CHECK(eq.class_id == "ED58BD725C60464EA63DD2F400748F6F");   // negative fields: their 32-bit patterns
        CHECK(eq.component == "\xAA\xBB" && eq.controller == "\xCC\xDD");

        CHECK(plugins[2].kind == "VST" && plugins[2].name == "OldDelay" && plugins[2].component.empty());
    }
    return finish_tests();
}
