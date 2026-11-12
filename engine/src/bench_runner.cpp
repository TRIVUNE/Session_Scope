// bench_runner.cpp - see bench_runner.h: starts the bench host on one plug-in and watches it.
//
// System calls used, and where they are documented (learn.microsoft.com):
//   CreateProcessW              starts the host, with its output going to a log file
//   CreateJobObjectW,           a "job" holds the host and anything it starts; closing or
//   AssignProcessToJobObject,   terminating the job ends them all (a plug-in may start helper
//   TerminateJobObject          programs, e.g. for its licence)
//   WaitForSingleObject         the watchdog: waits for the host, at most the timeout
//   GetExitCodeProcess          how the host ended (0: normally)

#include "bench_runner.h"

#include "install.h"
#include "json.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sessionscope {

namespace {

std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return file ? std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) : std::string();
}

// The last "stage: ..." line the host printed.
std::string last_stage(const fs::path &log) {
    std::istringstream lines(read_file(log));
    std::string line, stage;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("stage: ", 0) == 0) stage = line.substr(7);
    }
    return stage;
}

#ifdef _WIN32
// One argument for a Windows command line, quoted so the program gets it back exactly (the rules
// of CommandLineToArgvW: backslashes only matter right before a quote).
std::wstring quoted(const std::wstring &argument) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');   // before the closing quote
    return out + L"\"";
}

std::wstring number_text(double value) {
    std::wostringstream text;
    text << value;
    return text.str();
}
#endif

}  // namespace

fs::path bench_host_near_program() {
    const fs::path native = built_folder_near_program();
#ifdef _WIN32
    return native.empty() ? fs::path() : native / "sessionscope_bench_host.exe";
#else
    return native.empty() ? fs::path() : native / "sessionscope_bench_host";
#endif
}

namespace {

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Every installed .vst3 (file or bundle folder) in the standard VST3 folders and their subfolders.
std::vector<fs::path> installed_vst3() {
    std::vector<fs::path> roots, found;
#ifdef _WIN32
    if (const wchar_t *common = _wgetenv(L"CommonProgramFiles")) roots.push_back(fs::path(common) / "VST3");
    if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA")) roots.push_back(fs::path(local) / "Programs" / "Common" / "VST3");
#else
    roots.push_back("/Library/Audio/Plug-Ins/VST3");
    if (const char *home = std::getenv("HOME")) roots.push_back(fs::path(home) / "Library/Audio/Plug-Ins/VST3");
#endif
    for (const fs::path &root : roots) {
        std::error_code error;
        // A bundle is a folder named .vst3; its insides are not searched (they hold the same name again).
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error);
             !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (lower(it->path().extension().u8string()) != ".vst3") continue;
            found.push_back(it->path());
            if (it->is_directory(error)) it.disable_recursion_pending();
        }
    }
    return found;
}

// The words of a plug-in's name worth matching against file names: letters and digits, three or
// more long, lower case ("UADx LA-2A Tube Compressor" -> uadx, tube, compressor).
std::vector<std::string> name_words(const std::string &name) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : name + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (word.size() >= 3) words.push_back(word);
            word.clear();
        }
    }
    return words;
}

}  // namespace

fs::path find_vst3(const std::string &file_name) {
    const std::string wanted = lower(file_name);
    for (const fs::path &file : installed_vst3())
        if (lower(file.filename().u8string()) == wanted) return file;
    return {};
}

fs::path default_vst3_index() {
#ifdef _WIN32
    const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
    return local ? fs::path(local) / "SessionScope" / "vst3-index.tsv" : fs::path();
#else
    const char *home = std::getenv("HOME");
    return home ? fs::path(home) / "Library/Caches/SessionScope/vst3-index.tsv" : fs::path();
#endif
}

fs::path find_vst3_by_id(const fs::path &host, const std::string &class_id, const std::string &name_hint,
                         const fs::path &cache, const fs::path &work_folder) {
    // The cache: "path <tab> modification time <tab> id,id,..." per line. A file that changed
    // since (an update) is asked again.
    struct Known {
        long long modified = 0;
        std::string ids;
    };
    std::map<std::string, Known> known;
    {
        std::istringstream lines(read_file(cache));
        std::string line;
        while (std::getline(lines, line)) {
            const std::size_t a = line.find('\t'), b = line.find('\t', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            known[line.substr(0, a)] = Known{std::atoll(line.substr(a + 1, b - a - 1).c_str()), line.substr(b + 1)};
        }
    }
    const std::string wanted = lower(class_id);
    const auto modified = [](const fs::path &file) {
        std::error_code error;
        return static_cast<long long>(fs::last_write_time(file, error).time_since_epoch().count());
    };

    // Files whose names share words with the plug-in's name first; the rest after.
    std::vector<fs::path> files = installed_vst3();
    const std::vector<std::string> words = name_words(name_hint);
    const auto score = [&](const fs::path &file) {
        const std::string text = lower(file.u8string());
        int hits = 0;
        for (const std::string &w : words) hits += text.find(w) != std::string::npos;
        return hits;
    };
    std::stable_sort(files.begin(), files.end(), [&](const fs::path &a, const fs::path &b) { return score(a) > score(b); });

    for (const fs::path &file : files) {
        const std::string key = file.u8string();
        auto entry = known.find(key);
        if (entry == known.end() || entry->second.modified != modified(file)) {
            BenchRequest list;
            list.plugin = file;
            list.list_classes = true;
            list.timeout_seconds = 30;
            const BenchRun run = run_bench(host, list, work_folder, "list");
            std::string ids;
            if (const std::optional<JsonValue> result = parse_json(run.result_json)) {
                for (const JsonValue &c : (*result)["classes"].items()) ids += (ids.empty() ? "" : ",") + lower(c["id"].text());
            }
            known[key] = Known{modified(file), ids};   // a file that failed is kept too (no ids), so it isn't asked again
            std::error_code ignored;
            fs::create_directories(cache.parent_path(), ignored);
            std::ofstream(cache, std::ios::app) << key << '\t' << known[key].modified << '\t' << ids << '\n';
            entry = known.find(key);
        }
        if (entry->second.ids.find(wanted) != std::string::npos) return file;
    }
    return {};
}

BenchRun run_bench(const fs::path &host, const BenchRequest &request, const fs::path &work_folder,
                   const std::string &tag) {
    BenchRun run;
    std::error_code ignored;
    fs::create_directories(work_folder, ignored);
    const fs::path result = work_folder / fs::u8path(tag + ".json");
    run.log = work_folder / fs::u8path(tag + ".log");
    fs::remove(result, ignored);   // an old result must never be read as this run's

#ifdef _WIN32
    std::wstring command = quoted(host.wstring()) + L" " + quoted(request.plugin.wstring()) + L" --out " +
                           quoted(result.wstring()) + L" --rate " + number_text(request.rate) + L" --block " +
                           number_text(request.block) + L" --seconds " + number_text(request.seconds);
    if (!request.class_name.empty()) command += L" --class " + quoted(fs::u8path(request.class_name).wstring());
    if (!request.preset.empty()) command += L" --preset " + quoted(request.preset.wstring());
    if (request.program >= 0) command += L" --program " + number_text(request.program);
    if (!request.class_id.empty()) command += L" --class-id " + fs::u8path(request.class_id).wstring();
    if (!request.state_component.empty()) command += L" --state-component " + quoted(request.state_component.wstring());
    if (!request.state_controller.empty()) command += L" --state-controller " + quoted(request.state_controller.wstring());
    if (request.report_parameters) command += L" --report-parameters 1";
    if (request.list_classes) command += L" --list-classes 1";

    // The host's output and errors (and anything a plug-in prints) go to the log file. The handle
    // must be inheritable, so the new process can write to it.
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE log = CreateFileW(run.log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        run.outcome = "could not start";
        run.last_stage = "can't write the log file";
        return run;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    // Closing the job (even if this program dies) ends every process in it.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    // Started suspended, put into the job, then let go: so not even its first step runs outside the job.
    const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
                                        nullptr, &startup, &process) != 0;
    CloseHandle(log);   // the host has its own copy now
    if (!started) {
        if (job) CloseHandle(job);
        run.outcome = "could not start";
        run.last_stage = "the bench host did not start (is it built? " + host.u8string() + ")";
        return run;
    }
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);

    const auto timeout_ms = static_cast<DWORD>(request.timeout_seconds * 1000);
    const bool timed_out = WaitForSingleObject(process.hProcess, timeout_ms) == WAIT_TIMEOUT;
    if (timed_out) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);   // also ends any helper the plug-in left running

    run.exit_code = code;
    run.outcome = timed_out ? "timed out" : code == 0 ? "finished" : "crashed";
    // Exit codes 2 and 3 are the host's own "wrong arguments" and "the plug-in can't be used":
    // it ended on purpose and wrote why.
    if (!timed_out && (code == 2 || code == 3)) run.outcome = "finished";
#else
    (void)host;
    (void)request;
    run.outcome = "could not start";
    run.last_stage = "the bench runs on Windows only for now";
    return run;
#endif
    run.last_stage = last_stage(run.log);
    run.result_json = read_file(result);
    return run;
}

}  // namespace sessionscope
