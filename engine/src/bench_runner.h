// bench_runner.h - `sessionscope bench`: runs the offline bench on one plug-in, safely
//
// The bench itself is a separate program, build/native/sessionscope_bench_host.exe
// (native/bench/bench_host.cpp): it loads one plug-in outside any DAW, plays a test sound through
// it, times every block and checks that the plug-in really processed. This file starts that
// program and watches it, the way decision 0006 asks for:
//   - a separate process, so a plug-in that crashes takes only the bench host down;
//   - a watchdog: if the host is not done in time (a plug-in that hangs, or waits for someone to
//     click a licence window), it is killed, together with anything it started;
//   - Windows' crash window is switched off in the host, so a crash ends it at once.
// The host writes its result as a small JSON file; it also announces every step on its error
// output ("stage: load", "stage: measure"...), which goes to a log file, so after a crash the
// runner can say at which step it happened.
//
// Windows only for now (the bench host is Windows only, like the Probe it grew out of).
#pragma once

#include <filesystem>
#include <string>

namespace sessionscope {

struct BenchRequest {
    std::filesystem::path plugin;     // the .vst3 file or bundle folder
    std::string class_name;           // which plug-in in the file; empty: the first one
    std::filesystem::path preset;     // a saved .vstpreset to load first; empty: the plug-in's defaults
    int program = -1;                 // a factory program (preset) to choose by number; -1: none
    double rate = 48000;              // sample rate
    int block = 256;                  // block size, in samples
    double seconds = 10;              // how much sound to time (after one second of warm-up)
    double timeout_seconds = 180;     // the watchdog: kill the host after this long
};

struct BenchRun {
    // finished:        the host wrote its result and exited normally
    // crashed:         the host ended with an error code (a result may still be there, if the
    //                  crash came after the measurement, while unloading)
    // timed out:       the watchdog killed it
    // could not start: the bench host program is missing or would not start
    std::string outcome;
    unsigned long exit_code = 0;
    std::string last_stage;           // the host's last "stage:" line: where it was when it ended
    std::string result_json;          // the host's result file, empty if it wrote none
    std::filesystem::path log;        // everything the host (and the plug-in) printed
};

// The bench host that belongs to the running program (build/native next to build/engine).
std::filesystem::path bench_host_near_program();

// Runs the host on one plug-in. `work_folder` gets the result and log files, named after `tag`.
BenchRun run_bench(const std::filesystem::path &host, const BenchRequest &request,
                   const std::filesystem::path &work_folder, const std::string &tag);

}  // namespace sessionscope
