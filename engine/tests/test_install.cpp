// Tests for installing the plug-ins (engine/src/install.h). Everything happens in a scratch folder
// under the system's temporary folder: the real plug-in folders are never touched.

#include "install.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace sessionscope;

// Writes a small file, making its folder first.
static void write_file(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

// A fresh, empty scratch folder with a name no other run uses.
static fs::path scratch_folder() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("sessionscope_test_install_" + std::to_string(now));
    fs::create_directories(folder);
    return folder;
}

int main() {
    const fs::path scratch = scratch_folder();

    // Stand-ins for the built plug-ins, with the names the build gives them on this system.
    InstallPlaces places;
    places.built = scratch / "built";
    places.user_plugins = scratch / "user";
    places.shared_plugins = scratch / "shared";
    places.reaper_resource = scratch / "REAPER";
    const std::vector<PluginCopy> copies = plugin_copies(places);
    for (const PluginCopy &copy : copies) {
#ifdef __APPLE__
        write_file(copy.built / "Contents" / "MacOS" / "SessionScope", "plug-in " + copy.name);   // bundles are folders
#else
        write_file(copy.built, "plug-in " + copy.built.filename().string());
#endif
    }
    write_file(places.built / "reaper_sessionscope.dll", "extension");

    // ---- the list of copies: per-user first, each with something to remove
    CHECK(!copies.empty());
    CHECK(!copies.front().shared);
    CHECK(copies.back().shared);
    for (const PluginCopy &copy : copies) {
        CHECK(!copy.name.empty());
        CHECK(!copy.remove.empty());
    }

    // ---- same_contents
    write_file(scratch / "a.bin", "one");
    write_file(scratch / "b.bin", "one");
    write_file(scratch / "c.bin", "two");
    write_file(scratch / "d.bin", "longer");
    CHECK(same_contents(scratch / "a.bin", scratch / "b.bin"));
    CHECK(!same_contents(scratch / "a.bin", scratch / "c.bin"));     // same size, different bytes
    CHECK(!same_contents(scratch / "a.bin", scratch / "d.bin"));     // different size
    CHECK(!same_contents(scratch / "a.bin", scratch / "missing"));   // a missing file is never the same
    write_file(scratch / "f1" / "x" / "y.bin", "one");
    write_file(scratch / "f2" / "x" / "y.bin", "one");
    write_file(scratch / "f3" / "x" / "y.bin", "two");
    CHECK(same_contents(scratch / "f1", scratch / "f2"));            // folders: the same files inside
    CHECK(!same_contents(scratch / "f1", scratch / "f3"));
    CHECK(!same_contents(scratch / "f1", scratch / "a.bin"));        // a folder is never the same as a file

    // ---- REAPER not installed: the plug-ins go in, the extension is skipped, and that is a success
    CHECK(install_everything(places) == 0);
    for (const PluginCopy &copy : copies) CHECK(same_contents(copy.built, copy.target));
    CHECK(!fs::exists(places.reaper_resource));

    // ---- REAPER installed: the extension goes into its UserPlugins
    fs::create_directories(places.reaper_resource);
    CHECK(install_everything(places) == 0);                          // the plug-ins are already in place: still fine
    CHECK(same_contents(places.built / "reaper_sessionscope.dll", places.reaper_resource / "UserPlugins" / "reaper_sessionscope.dll"));

    // ---- a rebuilt plug-in replaces the installed copy
    const PluginCopy &first = copies.front();
#ifdef __APPLE__
    write_file(first.built / "Contents" / "MacOS" / "SessionScope", "a newer build");
#else
    write_file(first.built, "a newer build");
#endif
    CHECK(!same_contents(first.built, first.target));
    CHECK(install_everything(places) == 0);
    CHECK(same_contents(first.built, first.target));

    // ---- something not built: nothing is installed, and the command fails
    InstallPlaces empty = places;
    empty.built = scratch / "nothing-built";
    empty.user_plugins = scratch / "user2";
    CHECK(install_everything(empty) == 1);
    CHECK(!fs::exists(empty.user_plugins));

    // ---- uninstall removes every copy and the extension, and nothing else
    write_file(places.user_plugins / "VST3" / "Someone Else.vst3", "not ours");
    CHECK(uninstall_everything(places) == 0);
    for (const PluginCopy &copy : copies) CHECK(!fs::exists(copy.remove));
    CHECK(!fs::exists(places.reaper_resource / "UserPlugins" / "reaper_sessionscope.dll"));
    CHECK(fs::exists(places.user_plugins / "VST3" / "Someone Else.vst3"));
    CHECK(uninstall_everything(places) == 0);                        // nothing left: still fine

    // ---- the real folders are known on this system (only looked at, never written)
    const InstallPlaces real = default_install_places(places.built);
    CHECK(!real.user_plugins.empty());
    CHECK(!real.shared_plugins.empty());
    CHECK(real.user_plugins != real.shared_plugins);
    CHECK(!program_folder().empty());

    std::error_code error;
    fs::remove_all(scratch, error);
    return finish_tests();
}
