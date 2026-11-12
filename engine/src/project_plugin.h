// project_plugin.h - one plug-in found in a DAW project, with its saved state
//
// Filled by the project readers (rpp_reader.h for REAPER, als_reader.h for Ableton Live) and
// used by `sessionscope bench --project` to run each plug-in with the person's own settings.
#pragma once

#include <string>

namespace sessionscope {

struct ProjectPlugin {
    std::string track;          // the track's name ("" if it has none); "MASTER" for the master track
    int track_number = 0;       // 1 for the first track, 0 for the master
    std::string kind;           // "VST3", "VST" (VST2), "JS", "CLAP", "AU", "DX"...
    std::string name;           // as the DAW shows it: "VST3: Pro-Q 4 (FabFilter)", or "Pro-Q 4"
    std::string file;           // the plug-in's file name, "FabFilter Pro-Q 4.vst3" (no folder);
                                // empty when the project doesn't say (Live): found by class_id
    std::string class_id;       // VST3: the id as 32 hex digits, its four 32-bit numbers in order
    bool bypassed = false;      // switched off in the project
    bool offline = false;       // set offline in the project (not even loaded)
    std::string component;      // VST3: the audio part's saved state (raw bytes)
    std::string controller;     // VST3: the controller's saved state (raw bytes, often empty)
    std::string problem;        // why the state could not be read; empty when it was
};

}  // namespace sessionscope
