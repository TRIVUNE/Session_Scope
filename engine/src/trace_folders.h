#ifndef SESSIONSCOPE_TRACE_FOLDERS_H
#define SESSIONSCOPE_TRACE_FOLDERS_H

// DAW-neutral trace folder locations. Finds both the default SessionScope traces and older REAPER
// recordings. A missing or unreadable folder gives an empty result rather than an exception.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sessionscope {

// The default trace root for the current DAW. Uses SESSIONSCOPE_TRACES if set; otherwise
// <base>/SessionScope/traces where base is:
//   Windows: %APPDATA% (via _wgetenv for non-ASCII user names)
//   macOS: ~/Library/Application Support
//   Linux: ~/.config
std::filesystem::path default_trace_root();

// <base>/SessionScope, with the same base as default_trace_root(): where the HUD keeps its
// settings. Empty if the base folder is unknown.
std::filesystem::path settings_folder();

// The trace root used by the REAPER recorder script (hosts/reaper/Scripts/SessionScope/).
// <base>/REAPER/SessionScope/traces with the same base as default_trace_root().
std::filesystem::path reaper_trace_root();

// Subfolders of `root` that contain a sensor.csv, sorted by folder name ascending (folder names
// are date-times like 20261105-143022, so later names are newer recordings). A missing or
// non-directory root gives an empty list and never throws.
std::vector<std::filesystem::path> trace_folders_in(const std::filesystem::path &root);

// All trace folders to search. If `root` is given, only that root is used. Otherwise, combines
// default_trace_root() and reaper_trace_root(), sorted by folder name ascending. When
// SESSIONSCOPE_TRACES is set, only that folder is used (a test or user pointing at one place
// means exactly that place).
std::vector<std::filesystem::path> all_trace_folders(const std::optional<std::filesystem::path> &root);

// The newest trace folder (last by name sort), or nullopt if none.
std::optional<std::filesystem::path> latest_trace(const std::optional<std::filesystem::path> &root);

// Generates a new trace folder name using the current local time, matching the REAPER recorder's
// format: YYYYMMDD-HHMMSS (e.g., 20261105-143022). This sorts correctly alongside REAPER folders.
std::string new_trace_folder_name();

}  // namespace sessionscope

#endif  // SESSIONSCOPE_TRACE_FOLDERS_H
