// rpp_reader.h - the plug-ins in a REAPER project file (.rpp), with each one's saved state
//
// The offline session analyzer (decision 0006) measures every plug-in of a project with the
// settings the person saved. A DAW saves those settings by asking each plug-in for its state
// (VST3: IComponent::getState and IEditController::getState) and writing the bytes into the
// project. Whatever the plug-in's own preset format (.ffp, .SerumPreset...), this is what
// reopening the project hands back, so it is all the bench needs.
//
// An .rpp is plain text: nested blocks that open with "<NAME ..." and close with ">". A plug-in
// on a track looks like this (inside <TRACK ... <FXCHAIN):
//
//   BYPASS 0 0 0                                      bypassed?, offline?, (unused here)
//   <VST "VST3: Pro-Q 4 (FabFilter)" "FabFilter Pro-Q 4.vst3" 0 "" 1234{5653...} ""
//     base64 lines
//   >
//
// The base64, decoded, is three parts (worked out from REAPER 7.83 projects, 12 Nov 2026:
// docs/research/rpp-state.md):
//   1. REAPER's header: u32 id, u32 magic, u32 input pins, 8 bytes per input pin, u32 output
//      pins, 8 bytes per output pin, u32 size of part 2, u32 1, u32 (flags)
//   2. the state: u32 component size, u32 1, component bytes, u32 controller size, u32 1,
//      controller bytes
//   3. the preset name REAPER shows (not needed)
// All numbers are little-endian. Every base64 line has a length divisible by 4, so each line is
// decoded on its own and the bytes are joined.
#pragma once

#include "project_plugin.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sessionscope {

// Every plug-in in the project, in track order. Throws std::runtime_error if the file can't be read.
std::vector<ProjectPlugin> read_rpp_plugins(const std::filesystem::path &project);

// The same, from the project's text (for tests).
std::vector<ProjectPlugin> parse_rpp_plugins(const std::string &text);

// Decodes base64 text (whitespace ignored). Stops at the first character that isn't base64.
std::string decode_base64(const std::string &text);

}  // namespace sessionscope
