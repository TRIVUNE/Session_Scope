// als_reader.h - the plug-ins in an Ableton Live set (.als), with each one's saved state
//
// A Live set is gzip-compressed XML (inflate.h unpacks it). Live 12 writes a VST3 plug-in like
// this (seen in a set the person saved on 12 Nov 2026: docs/research/als-state.md):
//
//   <MidiTrack Id="12"> ... <Name> <EffectiveName Value="1-Vital" /> ...
//     <PluginDevice Id="0">
//       <On> <Manual Value="true" /> ...            the device's on/off switch
//       <PluginDesc>
//         <Vst3PluginInfo Id="0">
//           <Preset> <Vst3Preset Id="2"> ...
//             <ProcessorState> hex ... </ProcessorState>     the component's state (getState)
//             <ControllerState> hex ... </ControllerState>   the controller's (often empty: <ControllerState />)
//           </Vst3Preset> </Preset>
//           <Name Value="Vital" />
//           <Uid> <Fields.0 Value="1448301654" /> ... <Fields.3 Value="0" /> </Uid>
//
// The Uid fields are the four 32-bit numbers the plug-in's id is made of, as signed decimals.
// Live does not store the plug-in's file, so `file` stays empty: the bench finds it by its id.
// VST2 plug-ins (<VstPluginInfo>) and Audio Units (<AuPluginInfo>) are listed, without state.
#pragma once

#include "project_plugin.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sessionscope {

// Every plug-in in the set, in track order (inside racks too). Throws std::runtime_error if the
// file can't be read or unpacked.
std::vector<ProjectPlugin> read_als_plugins(const std::filesystem::path &set);

// The same, from the set's XML (already unpacked; for tests).
std::vector<ProjectPlugin> parse_als_plugins(const std::string &xml);

}  // namespace sessionscope
