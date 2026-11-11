// install.cpp - see install.h for what installing means and which folders it uses.
//
// System calls used, and where they are documented:
//   GetModuleFileNameW (Windows)        the running program's own path, learn.microsoft.com
//   _NSGetExecutablePath (macOS)        the same, <mach-o/dyld.h>
//   std::filesystem (C++17)             copying, comparing and removing files and folders

#include "install.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace sessionscope {

namespace {

// An environment variable as a path, or an empty path when it isn't set. Windows keeps these as
// wide text, so a user name with letters outside ASCII survives.
fs::path env_path(const char *name) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));   // names are ASCII
    const wchar_t *value = _wgetenv(wide_name.c_str());                                 // nullptr: not set
    return value ? fs::path(value) : fs::path();
#else
    const char *value = std::getenv(name);   // nullptr: not set
    return value ? fs::path(value) : fs::path();
#endif
}

// Paths in messages as UTF-8 text (the console is switched to UTF-8 in sessionscope.cpp).
std::string shown(const fs::path &path) { return path.u8string(); }

// A whole file as bytes; empty if it can't be read.
std::string file_bytes(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);                                          // binary: no newline changes
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The files inside a folder, as paths relative to it, sorted so two folders can be compared.
std::vector<fs::path> files_inside(const fs::path &folder) {
    std::vector<fs::path> files;
    std::error_code error;                                                               // collects failures instead of throwing
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) files.push_back(fs::relative(it->path(), folder, error));
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Copies a plug-in file, or a whole bundle folder, replacing what was there. Returns an empty
// text when it worked, or the system's explanation when it didn't.
std::string copy_plugin(const fs::path &built, const fs::path &target) {
    std::error_code error;
    fs::create_directories(target.parent_path(), error);                                 // the folder may not exist yet
    if (error) return error.message();
    if (fs::is_directory(built)) {                                                       // a bundle (macOS): replace the folder
        fs::remove_all(target, error);
        if (error) return error.message();
        fs::copy(built, target, fs::copy_options::recursive, error);
    } else {                                                                             // one file (Windows)
        fs::copy_file(built, target, fs::copy_options::overwrite_existing, error);
    }
    return error ? error.message() : std::string();
}

// One copy with its message. `needs_admin`: a failure is expected without administrator rights
// and gets a gentler message. Returns true if the copy is in place afterwards.
bool copy_and_report(const fs::path &built, const fs::path &target, const std::string &what, bool needs_admin) {
    if (same_contents(built, target)) {                                                  // nothing to do
        std::printf("%s: already installed (%s)\n", what.c_str(), shown(target).c_str());
        return true;
    }
    const std::string problem = copy_plugin(built, target);
    if (problem.empty()) {
        std::printf("%s: installed to %s\n", what.c_str(), shown(target).c_str());
        return true;
    }
    if (needs_admin) {
        std::printf("%s: not installed (needs administrator rights, or the file is in use).\n", what.c_str());
    } else {
        // The usual reason: the DAW has loaded the old copy, and Windows won't replace an open file.
        std::printf("%s: could not write %s (%s). If your DAW is open, close it and run this again.\n",
                    what.c_str(), shown(target).c_str(), problem.c_str());
    }
    return false;
}

// The REAPER extension: REAPER loads every reaper_*.dll in <its settings folder>/UserPlugins.
fs::path extension_built(const InstallPlaces &places) { return places.built / "reaper_sessionscope.dll"; }
fs::path extension_installed(const InstallPlaces &places) {
    return places.reaper_resource / "UserPlugins" / "reaper_sessionscope.dll";
}

}  // namespace

InstallPlaces default_install_places(const fs::path &built) {
    InstallPlaces places;
    places.built = built;
#ifdef _WIN32
    places.user_plugins = env_path("LOCALAPPDATA") / "Programs" / "Common";              // per-user VST3 and CLAP folders live here
    places.shared_plugins = env_path("CommonProgramFiles");                              // C:\Program Files\Common Files
    if (places.shared_plugins.empty()) places.shared_plugins = "C:\\Program Files\\Common Files";
    places.reaper_resource = env_path("APPDATA") / "REAPER";
#elif defined(__APPLE__)
    places.user_plugins = env_path("HOME") / "Library" / "Audio" / "Plug-Ins";
    places.shared_plugins = "/Library/Audio/Plug-Ins";
    places.reaper_resource = env_path("HOME") / "Library" / "Application Support" / "REAPER";
#else
    places.user_plugins = env_path("HOME");                                              // Linux: ~/.vst3 and ~/.clap would go here; not built yet
    places.shared_plugins = "/usr/lib";
    places.reaper_resource = env_path("HOME") / ".config" / "REAPER";
#endif
    return places;
}

fs::path program_folder() {
    fs::path program;
#ifdef _WIN32
    wchar_t buffer[32768];                                                               // the longest path Windows allows
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);                     // nullptr: this program
    if (length > 0 && length < 32768) program = fs::path(std::wstring(buffer, length));
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) program = fs::path(buffer);
#else
    std::error_code link_error;
    program = fs::read_symlink("/proc/self/exe", link_error);                            // Linux's link to the running program
#endif
    return program.empty() ? fs::path() : program.parent_path();
}

fs::path built_folder_near_program() {
    const fs::path folder = program_folder();
    if (folder.empty()) return {};
    std::error_code error;
    const fs::path native = fs::weakly_canonical(folder / ".." / "native", error);       // build/engine/../native
    return (!error && fs::is_directory(native)) ? native : fs::path();
}

std::vector<PluginCopy> plugin_copies(const InstallPlaces &places) {
    std::vector<PluginCopy> copies;
    for (const bool shared : {false, true}) {                                            // per-user copies first: they are the required ones
        const fs::path &base = shared ? places.shared_plugins : places.user_plugins;
        const std::string where = shared ? " (shared folder)" : " (per-user folder)";
#ifdef __APPLE__
        // macOS: every plug-in is a bundle (a folder), built into build/native/mac, copied whole.
        const fs::path mac = places.built / "mac";
        copies.push_back({"VST3 sensor" + where, mac / "SessionScope.vst3", base / "VST3" / "SessionScope.vst3",
                          base / "VST3" / "SessionScope.vst3", shared});
        copies.push_back({"CLAP sensor" + where, mac / "SessionScope.clap", base / "CLAP" / "SessionScope.clap",
                          base / "CLAP" / "SessionScope.clap", shared});
        copies.push_back({"Audio Unit sensor" + where, mac / "SessionScope.component",
                          base / "Components" / "SessionScope.component", base / "Components" / "SessionScope.component", shared});
#else
        // Windows: a VST3 is a folder named .vst3 with the DLL at Contents/x86_64-win inside, under
        // the same name; a CLAP is a single DLL named .clap.
        const fs::path vst3 = base / "VST3";
        copies.push_back({"VST3 sensor" + where, places.built / "SessionScope.vst3.dll",
                          vst3 / "SessionScope.vst3" / "Contents" / "x86_64-win" / "SessionScope.vst3",
                          vst3 / "SessionScope.vst3", shared});
        copies.push_back({"VST3 probe" + where, places.built / "SessionScopeProbe.vst3.dll",
                          vst3 / "SessionScopeProbe.vst3" / "Contents" / "x86_64-win" / "SessionScopeProbe.vst3",
                          vst3 / "SessionScopeProbe.vst3", shared});
        copies.push_back({"CLAP sensor" + where, places.built / "SessionScope.clap", base / "CLAP" / "SessionScope.clap",
                          base / "CLAP" / "SessionScope.clap", shared});
        copies.push_back({"CLAP probe" + where, places.built / "SessionScopeProbe.clap", base / "CLAP" / "SessionScopeProbe.clap",
                          base / "CLAP" / "SessionScopeProbe.clap", shared});
#endif
    }
    return copies;
}

bool same_contents(const fs::path &a, const fs::path &b) {
    std::error_code error;
    if (!fs::exists(a, error) || !fs::exists(b, error)) return false;
    const bool a_folder = fs::is_directory(a, error), b_folder = fs::is_directory(b, error);
    if (a_folder != b_folder) return false;                                              // a file is never the same as a folder
    if (a_folder) {
        const std::vector<fs::path> files_a = files_inside(a), files_b = files_inside(b);
        if (files_a != files_b) return false;                                            // different file names
        for (const fs::path &file : files_a) {
            if (!same_contents(a / file, b / file)) return false;
        }
        return true;
    }
    if (fs::file_size(a, error) != fs::file_size(b, error) || error) return false;       // cheap check first
    return file_bytes(a) == file_bytes(b);                                               // a few megabytes at most
}

int install_everything(const InstallPlaces &places) {
    const std::vector<PluginCopy> copies = plugin_copies(places);
    for (const PluginCopy &copy : copies) {                                              // nothing built: say how to build
        if (!fs::exists(copy.built)) {
            std::printf("Not built yet: %s\nBuild first: cmake --build --preset windows (or mac)\n", shown(copy.built).c_str());
            return 1;
        }
    }

    bool ok = true;           // every required copy is in place
    bool shared_ok = true;    // every optional (shared-folder) copy is in place
    for (const PluginCopy &copy : copies) {
        const bool done = copy_and_report(copy.built, copy.target, copy.name, copy.shared);
        if (copy.shared) shared_ok = shared_ok && done;
        else ok = ok && done;
    }
    if (!shared_ok) {
#ifdef __APPLE__
        std::printf("If your DAW does not list the plug-ins, run `sudo sessionscope install`.\n");
#else
        std::printf("If your DAW does not list the plug-ins, run `sessionscope install` from an administrator terminal.\n");
#endif
    }

    // The REAPER extension: optional, Windows only for now, and only if REAPER has a settings folder.
    const fs::path extension = extension_built(places);
    if (!fs::exists(extension)) {
        std::printf("REAPER extension: not built on this system, skipped.\n");
    } else if (!fs::is_directory(places.reaper_resource)) {
        std::printf("REAPER extension: REAPER is not installed (no settings folder at %s), skipped.\n",
                    shown(places.reaper_resource).c_str());
    } else {
        ok = copy_and_report(extension, extension_installed(places), "REAPER extension", false) && ok;
    }

    std::printf("%s\n", ok ? "Done. Restart your DAW; if it doesn't list the plug-ins, rescan them."
                           : "Done, with the notes above.");
    return ok ? 0 : 1;
}

int uninstall_everything(const InstallPlaces &places) {
    bool ok = true;
    std::vector<fs::path> targets;
    for (const PluginCopy &copy : plugin_copies(places)) targets.push_back(copy.remove);
    targets.push_back(extension_installed(places));
    for (const fs::path &target : targets) {
        std::error_code error;
        if (!fs::exists(target, error)) continue;                                        // never installed: nothing to say
        fs::remove_all(target, error);                                                   // a file or a whole bundle folder
        if (error) {
            std::printf("could not remove %s: %s\n", shown(target).c_str(), error.message().c_str());
            ok = false;
        } else {
            std::printf("removed %s\n", shown(target).c_str());
        }
    }
    return ok ? 0 : 1;
}

int install_command(int argc, char *argv[]) {
    bool uninstall = false;
    fs::path built = built_folder_near_program();
    for (int i = 2; i < argc; ++i) {                                                     // argv[1] is "install"
        const std::string option = argv[i];
        if (option == "--uninstall") {
            uninstall = true;
        } else if (option == "--from" && i + 1 < argc) {
            built = fs::u8path(argv[++i]);
        } else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n"
                                 "Usage: sessionscope install [--uninstall] [--from BUILD_NATIVE_FOLDER]\n", option.c_str());
            return 2;
        }
    }
    if (built.empty() && !uninstall) {                                                   // uninstalling needs no build
        std::fprintf(stderr, "Can't find the built plug-ins (build/native). Build first, or pass --from DIR.\n");
        return 1;
    }
    const InstallPlaces places = default_install_places(built);
    return uninstall ? uninstall_everything(places) : install_everything(places);
}

}  // namespace sessionscope
