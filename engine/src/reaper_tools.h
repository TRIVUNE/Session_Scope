#ifndef SESSIONSCOPE_REAPER_TOOLS_H
#define SESSIONSCOPE_REAPER_TOOLS_H

// =================================================================================================
// reaper_tools.h - the `sessionscope reaper ...` commands
// =================================================================================================
//
//   sessionscope reaper install     put the SessionScope JSFX and scripts into REAPER
//   sessionscope reaper uninstall   take them out again
//   sessionscope reaper validate    run the whole validation inside a real, isolated REAPER
//
// REAPER keeps its settings, effects and scripts in one "resource folder" (%APPDATA%\REAPER on
// Windows). Installing makes two folders in it:
//   <resource>/Effects/SessionScope   the JSFX (hosts/reaper/Effects/SessionScope here)
//   <resource>/Scripts/SessionScope   the Lua scripts (hosts/reaper/Scripts/SessionScope here)
// By default they are links to this repository (a "junction" on Windows, a symbolic link
// elsewhere), so an edit here is live in REAPER at once. --copy copies the files instead and
// leaves a marker file, so an uninstall knows the copy is ours. A folder this tool did not make
// is never touched.
//
// Validating starts REAPER with -cfgfile pointing into build/reaper-validate. REAPER then uses
// that folder as its resource folder: its own settings, the silent "dummy" audio device, links to
// this repository's JSFX and scripts. Your own REAPER setup is not read or changed. Inside REAPER,
// hosts/reaper/tests/integration/validate.lua does the work and writes result.json; this code
// prepares the folder, starts REAPER, waits, and prints what was found, then the analyser's report
// on each recording.
//
// Replaces hosts/reaper/tools/reaper_install.py and reaper_validate.py.

#include <filesystem>
#include <string>

#include "json.h"

namespace sessionscope {

// REAPER's resource folder for the current user (it may not exist: REAPER not installed).
std::filesystem::path default_reaper_resource();

// The repository this program was built from: two folders above build/engine, if it holds
// hosts/reaper. Empty otherwise.
std::filesystem::path repository_near_program();

// Links (or copies, when `copy`) the JSFX and scripts into `resource`. Returns an empty text when
// it worked, or what stopped it. `quiet`: print nothing on success.
std::string reaper_install(const std::filesystem::path &repo, const std::filesystem::path &resource, bool copy, bool quiet);

// Removes what reaper_install made in `resource`; folders it did not make are left alone (and said so).
void reaper_uninstall(const std::filesystem::path &repo, const std::filesystem::path &resource);

// Sets REAPER's audio device in <work>/reaper.ini to device number `mode` at 48 kHz / 256 samples,
// keeping every other line REAPER wrote. Makes the file if it is missing.
void write_reaper_audio_config(const std::filesystem::path &work, int mode);

// Makes the isolated REAPER look for VST plug-ins in `folder` only (build/native: the plug-ins
// just built), so the validation tests the fresh build without installing it, and third-party
// plug-ins stay out of it. To try other plug-ins in the isolated REAPER, add their folders to
// vstpath64 in build/reaper-validate/reaper.ini by hand; the next validation sets it back.
void write_reaper_plugin_folder(const std::filesystem::path &work, const std::filesystem::path &folder);

// The validation's findings as readable text, a few lines per step, from result.json.
std::string validation_summary(const JsonValue &result);

// `sessionscope reaper install|uninstall|validate [options]`. Returns the program's exit code.
int reaper_command(int argc, char *argv[]);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_REAPER_TOOLS_H
