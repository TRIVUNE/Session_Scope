// Trace folder location helpers. No exceptions, no crashes on missing directories.

#include "trace_folders.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <system_error>

namespace sessionscope {

namespace {

// Helper to get a Windows environment variable as a wide string.
#ifdef _WIN32
std::optional<std::wstring> get_wenv(const wchar_t *name) {
    size_t required = 0;
    errno_t err = _wgetenv_s(&required, nullptr, 0, name);
    if (err != 0 || required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    err = _wgetenv_s(&required, value.data(), required, name);
    if (err != 0) return std::nullopt;
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}
#endif

// Gets the base directory for the current platform.
// Windows: %APPDATA% (via _wgetenv_s for non-ASCII user names)
// macOS: ~/Library/Application Support
// Linux: ~/.config
std::filesystem::path get_base_dir() {
#ifdef _WIN32
    if (auto appdata = get_wenv(L"APPDATA")) {
        std::filesystem::path result(*appdata);
        if (!result.empty()) return result;
    }
    // Fallback: USERPROFILE\AppData\Roaming
    if (auto userprofile = get_wenv(L"USERPROFILE")) {
        std::filesystem::path result(*userprofile);
        if (!result.empty()) return result / "AppData" / "Roaming";
    }
    return std::filesystem::path();
#elif defined(__APPLE__)
    const char *home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / "Library" / "Application Support";
    return std::filesystem::path();
#else
    const char *home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".config";
    return std::filesystem::path();
#endif
}

}  // namespace

std::filesystem::path settings_folder() {
    const std::filesystem::path base = get_base_dir();
    return base.empty() ? base : base / "SessionScope";
}

namespace {

// Checks if SESSIONSCOPE_TRACES is set and returns it as a path.
std::optional<std::filesystem::path> get_env_trace_root() {
#ifdef _WIN32
    if (auto env = get_wenv(L"SESSIONSCOPE_TRACES")) {
        std::filesystem::path result(*env);
        if (!result.empty()) return result;
    }
    return std::nullopt;
#else
    const char *env = std::getenv("SESSIONSCOPE_TRACES");
    if (env && *env) return std::filesystem::path(env);
    return std::nullopt;
#endif
}

}  // namespace

std::filesystem::path default_trace_root() {
    if (auto env_root = get_env_trace_root()) return *env_root;
    auto base = get_base_dir();
    if (base.empty()) return std::filesystem::path();
    return base / "SessionScope" / "traces";
}

std::filesystem::path reaper_trace_root() {
    auto base = get_base_dir();
    if (base.empty()) return std::filesystem::path();
    return base / "REAPER" / "SessionScope" / "traces";
}

std::vector<std::filesystem::path> trace_folders_in(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return result;
    }

    // An explicit loop: a range-for would throw if a folder vanished during the scan.
    std::filesystem::directory_iterator entry(root, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && entry != end) {
        std::error_code entry_error;
        if (entry->is_directory(entry_error) && std::filesystem::exists(entry->path() / "sensor.csv", entry_error)) {
            result.push_back(entry->path());
        }
        entry.increment(ec);
    }

    std::sort(result.begin(), result.end(),
              [](const std::filesystem::path &a, const std::filesystem::path &b) {
                  return a.filename() < b.filename();
              });
    return result;
}

std::vector<std::filesystem::path> all_trace_folders(const std::optional<std::filesystem::path> &root) {
    if (root) {
        return trace_folders_in(*root);
    }

    // Check if SESSIONSCOPE_TRACES is set - if so, only use that one
    if (get_env_trace_root()) {
        return trace_folders_in(default_trace_root());
    }

    // Otherwise combine default and REAPER roots
    auto default_folders = trace_folders_in(default_trace_root());
    auto reaper_folders = trace_folders_in(reaper_trace_root());

    std::vector<std::filesystem::path> combined;
    combined.reserve(default_folders.size() + reaper_folders.size());
    combined.insert(combined.end(), default_folders.begin(), default_folders.end());
    combined.insert(combined.end(), reaper_folders.begin(), reaper_folders.end());

    std::sort(combined.begin(), combined.end(),
              [](const std::filesystem::path &a, const std::filesystem::path &b) {
                  return a.filename() < b.filename();
              });
    return combined;
}

std::optional<std::filesystem::path> latest_trace(const std::optional<std::filesystem::path> &root) {
    auto folders = all_trace_folders(root);
    if (folders.empty()) return std::nullopt;
    return folders.back();
}

std::string new_trace_folder_name() {
    // Format: YYYYMMDD-HHMMSS (matches util.stamp() in hosts/reaper/Scripts/SessionScope/lib/util.lua)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time_t_now);
#else
    localtime_r(&time_t_now, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return std::string(buf);
}

}  // namespace sessionscope
