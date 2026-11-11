#ifndef SESSIONSCOPE_INSTALL_H
#define SESSIONSCOPE_INSTALL_H

// =================================================================================================
// install.h - copying the built plug-ins to where DAWs look for them (`sessionscope install`)
// =================================================================================================
//
// A DAW finds plug-ins by scanning a few standard folders. Every plug-in format has two:
//   a per-user folder   needs no administrator rights, but not every DAW scans it
//   a shared folder     the format's standard place; writing there needs administrator rights
//
// Installing copies each built plug-in into both (the per-user copy must work, the shared copy is
// a bonus) and the REAPER extension into REAPER's own UserPlugins folder, if REAPER is installed.
// A copy that is already there and identical is left alone, so installing twice is harmless.
//
// What gets installed (built by native/CMakeLists.txt into build/native):
//   SessionScope Sensor   VST3 and CLAP (and Audio Unit on macOS): measures every audio block
//   SessionScope Probe    VST3 and CLAP (Windows): times a plug-in for the Task Manager
//   reaper_sessionscope   the REAPER extension (Windows): REAPER's realtime CPU per block
//
// Everything here takes its folders as arguments (InstallPlaces), so the tests can install into a
// scratch folder and never touch the real plug-in folders.
//
// Replaces tools/install_paths.py, tools/install_plugins.py and the install steps of
// tools/setup.py (the Python scripts this project used before the build moved to CMake).

#include <filesystem>
#include <string>
#include <vector>

namespace sessionscope {

// The folders an install reads from and writes to.
struct InstallPlaces {
    std::filesystem::path built;             // where the build put the plug-ins: build/native
    std::filesystem::path user_plugins;      // per-user base folder; "VST3", "CLAP"... go inside it
    std::filesystem::path shared_plugins;    // shared base folder, same layout
    std::filesystem::path reaper_resource;   // REAPER's settings folder (it holds UserPlugins)
};

// The real folders on this computer. `built` is the build/native folder to install from.
//   Windows: %LOCALAPPDATA%\Programs\Common and %CommonProgramFiles%; REAPER in %APPDATA%\REAPER
//   macOS:   ~/Library/Audio/Plug-Ins and /Library/Audio/Plug-Ins
InstallPlaces default_install_places(const std::filesystem::path &built);

// The folder the running program is in (build/engine). Empty if the system won't say.
std::filesystem::path program_folder();

// The build/native folder that belongs to the running program: the program sits in build/engine,
// so it is the "native" folder next to its own folder. Empty if that folder doesn't exist.
std::filesystem::path built_folder_near_program();

// One copy an install makes.
struct PluginCopy {
    std::string name;                // for messages: "VST3 sensor (per-user folder)"
    std::filesystem::path built;     // the built file, or bundle folder on macOS
    std::filesystem::path target;    // where the copy goes
    std::filesystem::path remove;    // what an uninstall deletes (the whole bundle folder)
    bool shared = false;             // true: the shared folder, which may need administrator rights
};

// Every plug-in copy for these places, per-user copies first.
std::vector<PluginCopy> plugin_copies(const InstallPlaces &places);

// True if `b` exists and holds exactly what `a` holds. For folders: the same files with the same
// contents. Used to skip copies that are already in place.
bool same_contents(const std::filesystem::path &a, const std::filesystem::path &b);

// Copies everything, printing one line per copy. Returns 0 if every required copy is in place
// (per-user plug-ins, and the REAPER extension when REAPER is installed), 1 otherwise.
int install_everything(const InstallPlaces &places);

// Removes everything install_everything puts in place. Things that aren't there are skipped.
// Returns 0, or 1 if something could not be removed (a DAW still has it open).
int uninstall_everything(const InstallPlaces &places);

// `sessionscope install [--uninstall] [--from DIR]`. Returns the program's exit code.
int install_command(int argc, char *argv[]);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_INSTALL_H
