// reaper_tools.cpp - see reaper_tools.h for what the three commands do.
//
// System calls used, and where they are documented:
//   CreateDirectoryW + DeviceIoControl(FSCTL_SET_REPARSE_POINT)   making a directory junction on
//       Windows; the data layout is "REPARSE_DATA_BUFFER, mount point" on learn.microsoft.com
//   GetFileAttributesW (FILE_ATTRIBUTE_REPARSE_POINT)             is this folder a link?
//   CreateProcessW / WaitForSingleObject / TerminateProcess       starting REAPER, waiting, stopping
//   posix_spawn / waitpid / kill                                  the same on macOS and Linux
//
// REAPER's command line (-cfgfile, -newinst, -nosplash, -noactivate, -new, a script to run) is
// from REAPER's own "reaper.exe -?" help; the [audioconfig] keys in reaper.ini are not documented
// and were found by trying (see find_dummy_mode).

#include "reaper_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#include "install.h"
#include "render.h"
#include "report.h"
#include "trace.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;   // this program's environment, handed on to REAPER
#endif

namespace fs = std::filesystem;

namespace sessionscope {

namespace {

// The two folders an install makes inside the resource folder, and where each comes from here.
constexpr const char *PARTS[] = {"Effects", "Scripts"};
constexpr const char *FOLDER = "SessionScope";
// A copied (not linked) install holds this file, so an uninstall can tell the copy is ours.
constexpr const char *MARKER = ".sessionscope-install";

std::string shown(const fs::path &path) { return path.u8string(); }   // paths in messages, as UTF-8

// A whole text file; empty if it can't be read.
std::string read_text(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_text(const fs::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);     // binary: write "\n" as it is
    file << text;
}

// A path as the system itself would write it: complete (not relative to the current folder),
// without "..", and on Windows with backslashes. Paths from the command line pass through here.
fs::path tidy(const fs::path &path) {
    if (path.empty()) return path;
    std::error_code error;
    fs::path full = fs::weakly_canonical(path, error);                 // "weakly": the path need not exist yet
    if (error) full = fs::absolute(path, error);
    return full.make_preferred();                                      // "/" becomes "\" on Windows
}

// ---- links ---------------------------------------------------------------------------------------

// Is this path a link to another folder (a junction or symbolic link), rather than a real folder?
bool is_link(const fs::path &path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);   // junctions and symlinks both
#else
    std::error_code error;
    return fs::is_symlink(path, error);
#endif
}

// Makes `link` a link to the folder `target`. False if the system refuses.
bool make_link(const fs::path &target, const fs::path &link) {
#ifdef _WIN32
    // A junction, not a symbolic link: Windows lets any user make junctions, while symbolic links
    // need administrator rights or Developer Mode. A junction is an empty folder with a "reparse
    // point" attached that names the real folder.
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const HANDLE folder = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // BACKUP_SEMANTICS: open a folder
    if (folder == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    // The full path with backslashes only: a path typed with "/" works in most of Windows, but a
    // junction that stores one leads nowhere.
    const std::wstring print_name = tidy(target).native();             // what Explorer shows
    const std::wstring substitute = L"\\??\\" + print_name;           // what the system follows ("\??\" = full NT path)
    const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print_name.size() * sizeof(wchar_t);
    // The buffer: an 8-byte header, four 16-bit offsets and lengths, then both names, each ending in a zero.
    std::vector<unsigned char> buffer(8 + 8 + substitute_bytes + 2 + print_bytes + 2, 0);
    const auto put16 = [&](std::size_t at, std::size_t value) {       // a 16-bit number, low byte first
        buffer[at] = static_cast<unsigned char>(value & 0xFF);
        buffer[at + 1] = static_cast<unsigned char>(value >> 8);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;                     // "this reparse point is a junction"
    std::memcpy(buffer.data(), &tag, 4);
    put16(4, buffer.size() - 8);                                      // length of everything after the header
    put16(8, 0);                                                      // substitute name starts at byte 0 of the names
    put16(10, substitute_bytes);
    put16(12, substitute_bytes + 2);                                  // print name follows it and its zero
    put16(14, print_bytes);
    std::memcpy(buffer.data() + 16, substitute.data(), substitute_bytes);
    std::memcpy(buffer.data() + 16 + substitute_bytes + 2, print_name.data(), print_bytes);
    DWORD returned = 0;
    const bool ok = DeviceIoControl(folder, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    nullptr, 0, &returned, nullptr);
    CloseHandle(folder);
    if (!ok) RemoveDirectoryW(link.c_str());                          // don't leave an empty folder behind
    return ok;
#else
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    return !error;
#endif
}

// Removes a link (never what it points to), or a real folder with everything in it.
bool remove_link_or_folder(const fs::path &path) {
    std::error_code error;
    if (is_link(path)) {
#ifdef _WIN32
        return RemoveDirectoryW(path.c_str()) != 0;                   // removes the junction only
#else
        return fs::remove(path, error);
#endif
    }
    fs::remove_all(path, error);
    return !error;
}

// Did we install this folder? A link is ours if it leads into this repository; a real folder is
// ours if it holds the marker file.
bool ours(const fs::path &path, const fs::path &repo) {
    std::error_code error;
    if (is_link(path)) {
        const fs::path real = fs::canonical(path, error);             // follows the link
        if (error) return true;                                       // a link to nowhere (the repository moved): safe to replace
        const fs::path relative = real.lexically_relative(fs::canonical(repo, error));
        return !error && !relative.empty() && *relative.begin() != "..";   // inside the repository
    }
    return fs::exists(path / MARKER, error);
}

// ---- running REAPER ------------------------------------------------------------------------------

struct Launched {
    bool finished = false;   // REAPER quit by itself, or (after a timeout) `done` said the work was finished
    double seconds = 0;      // how long it ran
};

// Starts an isolated REAPER that runs `script`, and waits until it quits or `timeout` seconds pass.
// On a timeout, `done` decides whether the work got finished anyway, and REAPER is stopped.
Launched launch_reaper(const fs::path &exe, const fs::path &work, const fs::path &script, double timeout,
                       const std::function<bool()> &done) {
    // -cfgfile: use this settings file (and its folder as the resource folder); -newinst: a second
    // REAPER even if one is open; -nosplash -noactivate: stay out of the way; -new: empty project.
    const std::vector<fs::path> arguments = {exe, "-cfgfile", work / "reaper.ini", "-newinst", "-nosplash",
                                             "-noactivate", "-new", script};
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    Launched result;
#ifdef _WIN32
    std::wstring command;                                             // Windows takes one line: each argument in quotes
    for (const fs::path &argument : arguments) command += L"\"" + argument.native() + L"\" ";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return result;
    while (elapsed() < timeout) {
        if (WaitForSingleObject(process.hProcess, 500) == WAIT_OBJECT_0) {   // waits up to half a second for it to quit
            result.finished = true;
            break;
        }
    }
    if (!result.finished) {
        result.finished = done();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);                 // give it ten seconds to be gone
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    std::vector<std::string> texts;                                   // keeps the argument texts alive
    for (const fs::path &argument : arguments) texts.push_back(argument.string());
    std::vector<char *> argv;
    for (std::string &text : texts) argv.push_back(text.data());
    argv.push_back(nullptr);                                          // the list ends with a null
    pid_t pid = 0;
    if (posix_spawn(&pid, texts[0].c_str(), nullptr, nullptr, argv.data(), environ) != 0) return result;
    while (elapsed() < timeout) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {                  // WNOHANG: just ask, don't wait
            result.finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!result.finished) {
        result.finished = done();
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
#endif
    result.seconds = elapsed();
    return result;
}

// A one-off script REAPER runs to say which audio device it opened; then it quits (action 40004).
constexpr const char *DEVICE_PROBE = R"lua(
local f = io.open(reaper.GetResourcePath() .. '/probe.txt', 'w')
local ok, mode = reaper.GetAudioDeviceInfo('MODE')
f:write(tostring(ok and mode or ''), '\n', tostring(reaper.Audio_IsRunning()), '\n', reaper.GetAppVersion(), '\n')
f:close()
reaper.Main_OnCommand(40004, 0)
)lua";

// REAPER's device numbers ([audioconfig] mode) are not documented, so the dummy device's number is
// found by trying these in turn. On REAPER 7 / Windows it is 4. The answer is kept in
// dummy-mode.txt. Returns -1 if none of them is the dummy device.
int find_dummy_mode(const fs::path &exe, const fs::path &work) {
    const fs::path cache = work / "dummy-mode.txt";
    if (fs::exists(cache)) return std::atoi(read_text(cache).c_str());
    const fs::path script = work / "probe.lua";
    write_text(script, DEVICE_PROBE);
    for (const int mode : {4, 5, 6, 7, 2}) {
        write_reaper_audio_config(work, mode);
        const fs::path answer = work / "probe.txt";
        std::error_code error;
        fs::remove(answer, error);
        // Up to ten minutes: the first run in a new folder scans every plug-in on the computer
        // before it runs the script (four minutes were not enough with a few hundred plug-ins).
        const Launched run = launch_reaper(exe, work, script, 600, [&] { return fs::exists(answer); });
        const std::string text = read_text(answer);
        const std::string device = text.substr(0, text.find('\n'));
        std::printf("  probe mode=%d: %s (%.0f s)\n", mode,
                    text.empty() ? "no answer (REAPER did not run the script)" : device.c_str(), run.seconds);
        std::fflush(stdout);
        if (text.empty()) return -1;
        std::string lower = device;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("dummy") != std::string::npos) {
            write_text(cache, std::to_string(mode));
            return mode;
        }
    }
    return -1;
}

// ---- the summary ---------------------------------------------------------------------------------

// A JSON value as short text for the summary: 512 (not 512.000000), true/false, "none" for null.
std::string plain(const JsonValue &value) {
    if (value.is_number()) {
        char text[32];
        std::snprintf(text, sizeof text, "%g", value.number());
        return text;
    }
    if (value.is_bool()) return value.boolean() ? "true" : "false";
    if (value.is_string()) return value.text();
    return "none";
}

// Is a JSON value "set"? (true, a number that isn't 0, a text that isn't empty)
bool truthy(const JsonValue &value) {
    return value.boolean() || (value.is_number() && value.number() != 0) || !value.text().empty();
}

// How many rows a CSV file has besides its header line; 0 if it is missing.
long csv_rows(const fs::path &path) {
    std::ifstream file(path);
    long lines = 0;
    for (std::string line; std::getline(file, line);) ++lines;
    return lines > 0 ? lines - 1 : 0;
}

// printf into a std::string, for building the summary line by line.
// The attribute lets the compiler check each call's arguments against its format text.
__attribute__((format(printf, 2, 3)))
void add(std::string &out, const char *format, ...) {
    char line[2048];
    va_list arguments;                                                 // the values after `format`
    va_start(arguments, format);
    std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    out += line;
}

int validate_command(int argc, char *argv[]);

}  // namespace

fs::path default_reaper_resource() {
    return default_install_places({}).reaper_resource;                // one place knows the per-system folders
}

fs::path repository_near_program() {
    const fs::path folder = program_folder();                         // build/engine
    if (folder.empty()) return {};
    std::error_code error;
    const fs::path repo = fs::weakly_canonical(folder / ".." / "..", error);
    return (!error && fs::is_directory(repo / "hosts" / "reaper")) ? repo : fs::path();
}

std::string reaper_install(const fs::path &repo, const fs::path &resource, bool copy, bool quiet) {
    for (const char *part : PARTS) {
        const fs::path source = repo / "hosts" / "reaper" / part / FOLDER;
        const fs::path target = resource / part / FOLDER;
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (fs::exists(target, error) || is_link(target)) {            // something is there already
            if (!ours(target, repo)) return shown(target) + " exists and was not installed by SessionScope; move it away first.";
            if (!remove_link_or_folder(target)) return "could not replace " + shown(target);
        }
        if (copy) {
            fs::copy(source, target, fs::copy_options::recursive, error);
            if (error) return "could not copy to " + shown(target) + ": " + error.message();
            write_text(target / MARKER, shown(repo));                  // marks the copy as ours
        } else if (!make_link(source, target)) {
            return "could not link " + shown(target) + " to " + shown(source);
        }
        if (!quiet) std::printf("%s %s\n", copy ? "copied" : "linked", shown(target).c_str());
    }
    return {};
}

void reaper_uninstall(const fs::path &repo, const fs::path &resource) {
    for (const char *part : PARTS) {
        const fs::path target = resource / part / FOLDER;
        std::error_code error;
        if (!fs::exists(target, error) && !is_link(target)) continue;  // not installed
        if (!ours(target, repo)) {
            std::printf("left %s alone: not installed by SessionScope\n", shown(target).c_str());
            continue;
        }
        std::printf("%s %s\n", remove_link_or_folder(target) ? "removed" : "could not remove", shown(target).c_str());
    }
}

namespace {

// Sets `want` (key, value pairs) in the [target] section of <work>/reaper.ini, keeping every other
// line REAPER wrote: a key that is there gets the new value, a missing key is added to the
// section, a missing section is added at the end. `target` in lower case.
void set_reaper_ini(const fs::path &work, const std::string &target, const std::vector<std::pair<std::string, std::string>> &want) {
    const auto wanted = [&](const std::string &key) -> const std::string * {   // the value we want for a key, or nullptr
        for (const auto &pair : want) {
            if (pair.first == key) return &pair.second;
        }
        return nullptr;
    };

    std::vector<std::string> lines;                                    // the file as it is now
    std::istringstream existing(read_text(work / "reaper.ini"));
    for (std::string line; std::getline(existing, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();     // Windows line ends
        lines.push_back(line);
    }
    if (lines.empty()) lines.push_back("[reaper]");                    // a new file needs REAPER's own section first

    std::vector<std::string> out;
    std::vector<std::string> seen;                                     // keys of ours already in the file
    std::string section;                                               // the [section] we are inside, lower case
    bool had_section = false;                                          // the file has a [target] section
    const auto add_missing = [&] {                                     // our keys the section didn't have
        for (const auto &pair : want) {
            if (std::find(seen.begin(), seen.end(), pair.first) == seen.end()) out.push_back(pair.first + "=" + pair.second);
        }
    };
    for (std::string line : lines) {
        const std::size_t first = line.find_first_not_of(" \t"), last = line.find_last_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? "" : line.substr(first, last - first + 1);
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            if (section == target) add_missing();                      // leaving our section
            section = trimmed.substr(1, trimmed.size() - 2);
            std::transform(section.begin(), section.end(), section.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            had_section = had_section || section == target;
        } else if (section == target && trimmed.find('=') != std::string::npos) {
            const std::string key = trimmed.substr(0, trimmed.find('='));
            if (const std::string *value = wanted(key)) {
                seen.push_back(key);
                line = key + "=" + *value;                             // replace the old value
            }
        }
        out.push_back(line);
    }
    if (section == target) {
        add_missing();                                                 // the file ended inside our section
    } else if (!had_section) {
        out.push_back("[" + target + "]");
        add_missing();
    }
    std::string text;
    for (const std::string &line : out) text += line + "\n";
    write_text(work / "reaper.ini", text);
}

}  // namespace

void write_reaper_audio_config(const fs::path &work, int mode) {
    // The dummy device's format: 48 kHz, 256 samples per block (a 5.33 ms deadline).
    set_reaper_ini(work, "audioconfig", {{"dummy_srate", "48000"}, {"dummy_blocksize", "256"}, {"mode", std::to_string(mode)}});
}

void write_reaper_plugin_folder(const fs::path &work, const fs::path &folder) {
    // vstpath64: the folders REAPER scans for VST plug-ins, separated by ";". We write ours;
    // REAPER adds the system's standard VST3 folders after it again when it starts, and when one
    // plug-in is in two folders it uses the first, so the fresh build wins over an installed copy.
    set_reaper_ini(work, "reaper", {{"vstpath64", shown(folder)}});
}

std::string validation_summary(const JsonValue &result) {
    std::string out;
    const JsonValue &device = result["device"];
    const std::string extension = result["host_extension"].text();
    add(out, "\nREAPER %s, audio: %s %s Hz / %s samples, running: %s, extension: %s\n", result["host"]["version"].text().c_str(),
        plain(device["mode"]).c_str(), plain(device["srate"]).c_str(), plain(device["bsize"]).c_str(),
        plain(device["audio_running"]).c_str(), extension.empty() ? "not loaded" : extension.c_str());

    // The steps by name. A later step with the same name replaces an earlier one.
    std::vector<std::string> order;                                    // step names in the order they ran
    std::map<std::string, const JsonValue *> steps;
    for (const JsonValue &step : result["steps"].items()) {
        const std::string name = step["step"].text();
        if (!steps.count(name)) order.push_back(name);
        steps[name] = &step;
    }
    static const JsonValue none;
    const auto step = [&](const char *name) -> const JsonValue & { return steps.count(name) ? *steps[name] : none; };

    const JsonValue &build = step("build fixtures");
    add(out, "Fixtures built: %s\n", build["built"].is_number() ? plain(build["built"]).c_str() : "0");
    for (const JsonValue &problem : build["problems"].items()) add(out, "  ! %s\n", problem.text().c_str());

    const JsonValue &inventory = step("F4 inventory");
    if (inventory.is_object()) {
        add(out, "Spike C, declared delay of the Fixed Latency JSFX (slider vs what REAPER reports):\n");
        for (const JsonValue &check : inventory["fixed_latency"].items()) {
            const char *state = !check["enabled"].boolean() ? "bypassed" : (check["offline"].boolean() ? "offline" : "active");
            const bool match = check["pdc"].is_number() && check["pdc"].number() == check["slider"].number();
            add(out, "  %-36s %-8s slider %6.0f  reported %6s  %s\n", check["where"].text().c_str(), state,
                check["slider"].number(0), plain(check["pdc"]).c_str(), match ? "match" : "differs");
        }
        add(out, "  container children seen: %s\n", plain(inventory["container_children"]).c_str());
        std::vector<std::string> capabilities = inventory["capabilities"].keys();
        std::sort(capabilities.begin(), capabilities.end());
        std::string list;
        for (const std::string &name : capabilities) {
            if (truthy(inventory["capabilities"][name])) list += (list.empty() ? "" : ", ") + name;
        }
        add(out, "  capabilities: %s\n", list.c_str());
    }

    const JsonValue &bypass = step("F4 bypass");
    if (bypass.is_object()) {
        const JsonValue &pdc = bypass["pdc"];
        add(out, "  bypass test (slider 512): enabled and running %s, right after bypass %s, 1.5 s later %s\n",
            plain(pdc["enabled"]).c_str(), plain(pdc["just_bypassed"]).c_str(), plain(pdc["bypassed_after_1_5_s"]).c_str());
    }

    const JsonValue &identity = step("identity");                      // spike A: GUIDs across edits and save + reopen
    if (identity.is_object()) {
        add(out, "Spike A, identity stability: %s\n", identity["passed"].boolean() ? "passed" : "FAILED");
        std::vector<std::string> names = identity["checks"].keys();
        std::sort(names.begin(), names.end());
        for (const std::string &name : names) add(out, "  %s %s\n", identity["checks"][name].boolean() ? "ok  " : "FAIL", name.c_str());
    }

    const JsonValue &probe = step("probe");                            // the SessionScope Probe wrapping a plug-in
    if (probe.is_object()) {
        add(out, "Probe (Task Manager): %s\n", probe["passed"].boolean() ? "passed" : "FAILED");
        std::vector<std::string> names = probe["checks"].keys();
        std::sort(names.begin(), names.end());
        for (const std::string &name : names) add(out, "  %s %s\n", probe["checks"][name].boolean() ? "ok  " : "FAIL", name.c_str());
    }

    for (const std::string &name : order) {                            // steps that were skipped, and why
        const std::string ending = "skipped";
        if (name.size() >= ending.size() && name.compare(name.size() - ending.size(), ending.size(), ending) == 0) {
            add(out, "%s: %s\n", name.c_str(), (*steps[name])["reason"].text().c_str());
        }
    }

    const JsonValue &traces = result["traces"];
    for (const std::string &id : traces.keys()) {
        const JsonValue &summary = traces[id]["summary"];
        const long host_rows = csv_rows(fs::u8path(traces[id]["folder"].text()) / "host.csv");
        add(out, "Trace %s: %s sensor records, %s dropped, %s xruns, sensor %s, second sensor %s, %ld REAPER blocks timed by the extension\n",
            id.c_str(), plain(summary["sensor_records"]).c_str(), plain(summary["dropped_records"]).c_str(),
            plain(summary["audio_xruns"]).c_str(), truthy(summary["sensor_connected"]) ? "seen" : "NOT seen",
            truthy(summary["second_sensor"]) ? "reported" : "not reported", host_rows);
    }
    for (const JsonValue &error : result["errors"].items()) add(out, "ERROR %s\n", error.text().c_str());
    return out;
}

namespace {

// Prints the analyser's report on one recording, the way `sessionscope report` does, without colour.
void print_report(const fs::path &folder, const char *metric) {
    try {
        TraceReader trace(folder);
        trace.update();
        const Style style(false);                                      // no colour codes: the output is often saved
        std::printf("%s\n", report_text(trace, metric, DEFAULT_MIN_BLOCKS, DEFAULT_Q, &style).c_str());
    } catch (const std::exception &e) {
        std::printf("No report for %s: %s\n", shown(folder).c_str(), e.what());
    }
    std::fflush(stdout);
}

int validate_command(int argc, char *argv[]) {
#ifdef _WIN32
    fs::path reaper = "C:\\Program Files\\REAPER (x64)\\reaper.exe";
#elif defined(__APPLE__)
    fs::path reaper = "/Applications/REAPER.app/Contents/MacOS/REAPER";
#else
    fs::path reaper = "/opt/REAPER/reaper";
#endif
    fs::path repo = repository_near_program();
    fs::path work;
    double timeout = 600;                                              // above validate.lua's own 9-minute watchdog
    bool keep = false;
    for (int i = 3; i < argc; ++i) {                                   // argv[1] "reaper", argv[2] "validate"
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--reaper" && has_value) reaper = tidy(fs::u8path(argv[++i]));
        else if (option == "--repo" && has_value) repo = tidy(fs::u8path(argv[++i]));
        else if (option == "--work" && has_value) work = tidy(fs::u8path(argv[++i]));
        else if (option == "--timeout" && has_value) timeout = std::atof(argv[++i]);
        else if (option == "--keep") keep = true;
        else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n", option.c_str());
            return 2;
        }
    }
    if (repo.empty()) {
        std::fprintf(stderr, "Can't find the repository (hosts/reaper); pass --repo DIR.\n");
        return 1;
    }
    if (!fs::exists(reaper)) {
        std::fprintf(stderr, "REAPER not found at %s; pass --reaper PATH.\n", shown(reaper).c_str());
        return 1;
    }
    if (work.empty()) work = repo / "build" / "reaper-validate";
    const fs::path script = repo / "hosts" / "reaper" / "tests" / "integration" / "validate.lua";
    const fs::path extension = repo / "build" / "native" / "reaper_sessionscope.dll";

    // The folder stays between runs, so REAPER's slow first plug-in scan happens once; what
    // SessionScope wrote there last time is cleared (unless --keep).
    std::error_code error;
    fs::create_directories(work, error);
    if (!keep) fs::remove_all(work / "SessionScope", error);
    const std::string problem = reaper_install(repo, work, false, true);
    if (!problem.empty()) {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 1;
    }

    // The extension goes into the isolated REAPER's own UserPlugins; a stale copy is removed.
    const fs::path extension_copy = work / "UserPlugins" / extension.filename();
    const bool has_extension = fs::exists(extension);
    if (has_extension) {
        fs::create_directories(extension_copy.parent_path(), error);
        fs::copy_file(extension, extension_copy, fs::copy_options::overwrite_existing, error);
    } else {
        fs::remove(extension_copy, error);
    }

    std::printf("Isolated REAPER resource folder: %s\n", shown(work).c_str());
    std::printf("Extension: %s\n", has_extension ? "loading reaper_sessionscope.dll" : "not built");
    std::fflush(stdout);
    write_reaper_plugin_folder(work, repo / "build" / "native");       // the plug-ins just built, ahead of any installed copy
    int mode = find_dummy_mode(reaper, work);
    if (mode < 0) {
        std::printf("Could not select REAPER's dummy audio device; continuing with mode 4. Audible fixtures will be skipped.\n");
        mode = 4;
    }
    write_reaper_audio_config(work, mode);

    const fs::path result_path = work / "SessionScope" / "validation" / "result.json";
    const auto read_result = [&] { return parse_json(read_text(result_path)).value_or(JsonValue()); };
    // validate.lua saves result.json after every step and sets "complete" only in its last one.
    const auto finished = [&] { return read_result()["complete"].boolean(); };

    std::printf("Running hosts/reaper/tests/integration/validate.lua (about five minutes)...\n");
    std::fflush(stdout);
    const Launched run = launch_reaper(reaper, work, script, timeout, finished);
    if (!fs::exists(result_path)) {
        std::fprintf(stderr, "REAPER ran for %.0f s but wrote no result: the script never started (a dialog may be waiting).\n", run.seconds);
        return 1;
    }
    const JsonValue result = read_result();
    std::string device = result["device"]["mode"].text();
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (device.find("dummy") != std::string::npos) write_text(work / "dummy-mode.txt", std::to_string(mode));   // skip the probe next time
    std::printf("%s", validation_summary(result).c_str());
    if (!run.finished) std::printf("\nREAPER was stopped after %.0f s before the validation finished.\n", run.seconds);

    // The analyser's full report on each recording (T10 is only about the second-sensor warning).
    for (const std::string &id : result["traces"].keys()) {
        if (id == "T10") continue;
        const fs::path folder = fs::u8path(result["traces"][id]["folder"].text());
        std::printf("\n====================================================================================================\n");
        print_report(folder, "lateness");
        if (id.rfind("F7", 0) == 0) {                                  // the known load next to it, for reading S2 by eye
            std::printf("\n");
            print_report(folder, "workload");
        }
    }
    return run.finished && result["errors"].items().empty() ? 0 : 1;
}

}  // namespace

int reaper_command(int argc, char *argv[]) {
    constexpr const char *usage =
        "Usage:\n"
        "  sessionscope reaper install [--copy] [--resource DIR] [--repo DIR]   link the JSFX and scripts into REAPER\n"
        "  sessionscope reaper uninstall [--resource DIR] [--repo DIR]          remove them\n"
        "  sessionscope reaper validate [--reaper PATH] [--timeout SECONDS] [--work DIR] [--keep] [--repo DIR]\n"
        "                                                                       run the validation in an isolated REAPER\n";
    const std::string action = argc > 2 ? argv[2] : "";
    if (action == "validate") return validate_command(argc, argv);
    if (action != "install" && action != "uninstall") {
        std::fprintf(stderr, "%s", usage);
        return 2;
    }

    fs::path resource = default_reaper_resource();
    fs::path repo = repository_near_program();
    bool copy = false;
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        const bool has_value = i + 1 < argc;
        if (option == "--resource" && has_value) resource = tidy(fs::u8path(argv[++i]));
        else if (option == "--repo" && has_value) repo = tidy(fs::u8path(argv[++i]));
        else if (option == "--copy" && action == "install") copy = true;
        else {
            std::fprintf(stderr, "Unknown or incomplete option: %s\n%s", option.c_str(), usage);
            return 2;
        }
    }
    if (repo.empty()) {
        std::fprintf(stderr, "Can't find the repository (hosts/reaper); pass --repo DIR.\n");
        return 1;
    }
    if (!fs::is_directory(resource)) {
        std::fprintf(stderr, "No REAPER resource folder at %s; pass --resource DIR.\n", shown(resource).c_str());
        return 1;
    }
    if (action == "uninstall") {
        reaper_uninstall(repo, resource);
        return 0;
    }
    const std::string problem = reaper_install(repo, resource, copy, false);
    if (!problem.empty()) {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 1;
    }
    std::printf("In REAPER: Actions > Show action list > New action > Load ReaScript, then pick the SessionScope scripts.\n");
    return 0;
}

}  // namespace sessionscope
