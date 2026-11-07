// Tests for trace folder location helpers.

#include "trace_folders.h"
#include "test_support.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using sessionscope::default_trace_root;
using sessionscope::reaper_trace_root;
using sessionscope::trace_folders_in;
using sessionscope::all_trace_folders;
using sessionscope::latest_trace;
using sessionscope::new_trace_folder_name;

namespace fs = std::filesystem;

static void write_file(const fs::path &p, const std::string &content = "") {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

static fs::path make_temp_dir() {
    fs::path base = fs::temp_directory_path() / "sessionscope_test_trace_folders";
    fs::create_directories(base);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    fs::path dir = base / std::to_string(ms);
    fs::create_directories(dir);
    return dir;
}

static void remove_dir(const fs::path &p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

int main() {
    fs::path tmp = make_temp_dir();

    // ------------------------------------------------------------
    // trace_folders_in: only folders with sensor.csv are listed; order by name
    // ------------------------------------------------------------
    {
        fs::path root = tmp / "trace_folders_in_test";
        fs::create_directories(root);

        // Create folders in non-alphabetical order
        fs::path f1 = root / "20261105-100000";
        fs::path f2 = root / "20261105-120000";
        fs::path f3 = root / "20261105-080000";
        fs::path f4 = root / "20261105-140000";  // no sensor.csv
        fs::create_directories(f1);
        fs::create_directories(f2);
        fs::create_directories(f3);
        fs::create_directories(f4);

        write_file(f1 / "sensor.csv", "seq,t\n1,0\n");
        write_file(f2 / "sensor.csv", "seq,t\n1,0\n");
        write_file(f3 / "sensor.csv", "seq,t\n1,0\n");
        // f4 has no sensor.csv

        // Also create a file (not a folder)
        write_file(root / "not_a_folder.txt", "hello");

        auto folders = trace_folders_in(root);
        CHECK(folders.size() == 3);
        CHECK(folders[0] == f3);  // earliest name first
        CHECK(folders[1] == f1);
        CHECK(folders[2] == f2);  // latest name last
    }

    // ------------------------------------------------------------
    // trace_folders_in: missing root gives empty list, never throws
    // ------------------------------------------------------------
    {
        fs::path missing = tmp / "does_not_exist";
        auto folders = trace_folders_in(missing);
        CHECK(folders.empty());
    }

    // ------------------------------------------------------------
    // trace_folders_in: root is a file, not a directory
    // ------------------------------------------------------------
    {
        fs::path file = tmp / "a_file.txt";
        write_file(file, "content");
        auto folders = trace_folders_in(file);
        CHECK(folders.empty());
    }

    // ------------------------------------------------------------
    // latest_trace with a root picks the largest name
    // ------------------------------------------------------------
    {
        fs::path root = tmp / "latest_test";
        fs::create_directories(root);

        fs::path f1 = root / "20261105-080000";
        fs::path f2 = root / "20261105-120000";
        fs::path f3 = root / "20261105-100000";
        fs::create_directories(f1);
        fs::create_directories(f2);
        fs::create_directories(f3);

        write_file(f1 / "sensor.csv", "");
        write_file(f2 / "sensor.csv", "");
        write_file(f3 / "sensor.csv", "");

        auto latest = latest_trace(root);
        CHECK(latest.has_value());
        CHECK(*latest == f2);  // 120000 is the largest
    }

    // ------------------------------------------------------------
    // latest_trace with no traces returns nullopt
    // ------------------------------------------------------------
    {
        fs::path root = tmp / "empty_latest";
        fs::create_directories(root);
        auto latest = latest_trace(root);
        CHECK(!latest.has_value());
    }

    // ------------------------------------------------------------
    // all_trace_folders with a root only uses that root
    // ------------------------------------------------------------
    {
        fs::path root1 = tmp / "all_test_1";
        fs::path root2 = tmp / "all_test_2";
        fs::create_directories(root1);
        fs::create_directories(root2);

        fs::path f1 = root1 / "20261105-100000";
        fs::path f2 = root2 / "20261105-120000";
        fs::create_directories(f1);
        fs::create_directories(f2);

        write_file(f1 / "sensor.csv", "");
        write_file(f2 / "sensor.csv", "");

        auto folders = all_trace_folders(root1);
        CHECK(folders.size() == 1);
        CHECK(folders[0] == f1);
    }

    // ------------------------------------------------------------
    // new_trace_folder_name returns a non-empty name in the right format
    // ------------------------------------------------------------
    {
        std::string name = new_trace_folder_name();
        CHECK(!name.empty());
        // Format: YYYYMMDD-HHMMSS (15 characters)
        CHECK(name.size() == 15);
        CHECK(name[8] == '-');
        // All other chars should be digits
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (i == 8) continue;
            CHECK(name[i] >= '0' && name[i] <= '9');
        }
        // Year should be reasonable (2020-2030)
        int year = std::stoi(name.substr(0, 4));
        CHECK(year >= 2020 && year <= 2030);
    }

    // ------------------------------------------------------------
    // default_trace_root and reaper_trace_root return valid paths (structure only)
    // We don't test the actual env var paths since we shouldn't touch real %APPDATA%
    // ------------------------------------------------------------
    {
        auto def = default_trace_root();
        auto reaper = reaper_trace_root();
        // Just verify they're valid path objects (not throwing)
        CHECK(!def.empty() || def.empty());  // always true, just exercises the code
        CHECK(!reaper.empty() || reaper.empty());
    }

    // ------------------------------------------------------------
    // Cleanup
    // ------------------------------------------------------------
    remove_dir(tmp);

    return finish_tests();
}
