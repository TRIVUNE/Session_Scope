// SessionScope REAPER extension: times REAPER's own processing of every audio block
// (decision 0003, capability metric.host_rt).
//
// How it works, in three parts:
//
//  1. Audio thread (on_audio). REAPER calls OnAudioBuffer twice per device block: once before it
//     processes the block (isPost = false) and once after (isPost = true). The time between the
//     two calls is REAPER's realtime work for that block. The audio thread only reads the clock
//     and writes one fixed-size record into a preallocated ring: no allocation, locks, file I/O or
//     REAPER API calls other than time_precise (blueprint 9.4). If the ring is full the record is
//     dropped and counted, never waited for.
//
//  2. Main thread (on_timer, ~30 times a second). Drains the ring. While the SessionScope recorder
//     script runs, it appends the records to host.csv in that trace's folder; the script announces
//     the folder through ExtState "SessionScope"/"host_trace_dir" and clears it when it stops.
//     It also copies every record into the shared-memory ring "SessionScope_host_v1", recording
//     or not, so the HUD window (engine/src/hud_engine.cpp, LiveHost) shows realtime CPU live.
//
//  3. Load/unload (ReaperPluginEntry). Finds the API functions, registers the hook and the timer,
//     and announces itself through ExtState "SessionScope"/"host_ext" so the recorder notes it.
//
// Uses only documented API: Audio_RegHardwareHook, plugin_register "timer", GetExtState,
// SetExtState, time_precise (reaper_plugin.h, reaper_plugin_functions.h, reaper-sdk b2ca1ec).
// Validated in REAPER 7.77 (docs/research/D-timing.md): on fixture F7b its per-bar load tracks the
// Workload JSFX's known cost within about 1.5 percentage points.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "reaper_plugin.h"

namespace {

const char *const VERSION = "0.2.0";
const char *const SECTION = "SessionScope";   // ExtState section shared with the Lua recorder

// Shared-memory ring for the HUD. Same publishing protocol as the VST3 sensor's ring: fill a
// record, then raise the count. All slots are doubles.
//   header  0 magic  1 layout  2 records written (published last)  3 capacity  4 slots per record
//           5 heartbeat (seconds, QueryPerformanceCounter: Python's perf_counter clock)  6 blocks lost
//           7 time_precise() at the same moment as 5: the reader converts record times to the
//             QPC clock (the VST3 sensor's) as t + (slot 5 - slot 7), never assuming the clocks agree
//   record  0 t (time_precise)  1 REAPER's time on the block (us)  2 frames  3 sample rate
// Written by the main thread only (on_timer), so the audio thread pays nothing for it.
constexpr double SHM_MAGIC = 53530002;
constexpr double SHM_LAYOUT = 1;
constexpr int SHM_HDR = 8;                       // header slots before the first record
constexpr int SHM_REC = 4;                       // slots per record
constexpr uint64_t SHM_CAP = 16384;              // about 90 s at 48 kHz / 256; the HUD reads every 0.1 s
constexpr const char *SHM_NAME = "SessionScope_host_v1";

// One timed audio block. 32 bytes, so the audio thread's store is a few instructions.
struct Block {
    double t;        // time_precise() when REAPER started the block (same clock as the sensor JSFX)
    double dur;      // seconds REAPER spent on it
    int frames;      // block length in samples
    double srate;    // sample rate
};

// Single-producer, single-consumer ring: the audio thread only advances g_head, the main thread
// only advances g_tail. Indices grow forever and are reduced modulo CAP when used, so "full" is
// simply head - tail == CAP. CAP is a power of two, so the modulo compiles to a mask.
constexpr uint64_t CAP = 1 << 16;   // about 6 minutes of 256-sample blocks at 48 kHz
Block g_ring[CAP];
std::atomic<uint64_t> g_head{0};    // next slot the audio thread writes
std::atomic<uint64_t> g_tail{0};    // next slot the main thread reads
std::atomic<uint64_t> g_lost{0};    // blocks dropped because the ring was full
double g_pre = 0;                   // start time of the current block; audio thread only

// REAPER API functions, looked up by name at load time.
double (*time_precise)() = nullptr;
int (*Audio_RegHardwareHook)(bool isAdd, audio_hook_register_t *reg) = nullptr;
const char *(*GetExtState)(const char *section, const char *key) = nullptr;
void (*SetExtState)(const char *section, const char *key, const char *value, bool persist) = nullptr;
int (*Register)(const char *name, void *infostruct) = nullptr;

// Audio thread. Keep this as small as it is: it runs inside every audio block.
void on_audio(bool isPost, int len, double srate, audio_hook_register_t *) {
    if (!isPost) {
        g_pre = time_precise();
        return;
    }
    const double t1 = time_precise();
    const uint64_t head = g_head.load(std::memory_order_relaxed);
    if (head - g_tail.load(std::memory_order_acquire) >= CAP) {   // full: drop, never wait
        g_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ring[head % CAP] = Block{g_pre, t1 - g_pre, len, srate};
    g_head.store(head + 1, std::memory_order_release);   // publish: the record is complete
}

// Registration record for the hook; REAPER fills in the channel fields we do not use.
audio_hook_register_t g_hook = {on_audio, nullptr, nullptr, 0, 0, nullptr};

#ifdef _WIN32
// The HUD ring, mapped on first use and kept until REAPER exits (the OS frees it). Main thread only.
volatile double *g_shm = nullptr;
uint64_t g_shm_n = 0;                            // records published so far

// Seconds on QueryPerformanceCounter, the clock the HUD's heartbeat check compares with.
double qpc_seconds() {
    static const double inv = [] {               // computed once: 1 / ticks per second
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);                 // current tick count
    return static_cast<double>(c.QuadPart) * inv;
}

// Create (or open, if the HUD made it first) the mapping and write a fresh header.
void open_shm() {
    constexpr size_t bytes = (SHM_HDR + SHM_CAP * SHM_REC) * sizeof(double);
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), SHM_NAME);
    if (!h) return;                              // no shared memory: the HUD just won't show CPU
    g_shm = static_cast<volatile double *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (!g_shm) return;
    g_shm[2] = 0;                                // a fresh ring: nothing published yet
    g_shm[1] = SHM_LAYOUT;
    g_shm[3] = static_cast<double>(SHM_CAP);
    g_shm[4] = SHM_REC;
    g_shm[0] = SHM_MAGIC;                        // last: the header is valid once this is set
}

// Copy one block into the HUD ring and publish it.
void publish(const Block &b) {
    volatile double *p = g_shm + SHM_HDR + (g_shm_n % SHM_CAP) * SHM_REC;   // the slot for record g_shm_n
    p[0] = b.t;                                  // when REAPER started the block
    p[1] = b.dur * 1e6;                          // how long it took, in microseconds
    p[2] = b.frames;                             // block length
    p[3] = b.srate;                              // sample rate: with frames, gives the deadline
    std::atomic_thread_fence(std::memory_order_release);   // the fields land before the count
    g_shm[2] = static_cast<double>(++g_shm_n);             // publish
}
#endif

// host.csv of the trace being recorded, if any. Main thread only.
std::string g_dir;
FILE *g_file = nullptr;
double g_last_flush = 0;

void close_file() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_dir.clear();
}

void open_file(const std::string &dir) {
    close_file();
    std::string path = dir + "/host.csv";
#ifdef _WIN32
    // REAPER passes UTF-8; trace folders may contain non-ASCII characters, so use the wide API.
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], n);
    g_file = _wfopen(wpath.c_str(), L"wb");
#else
    g_file = fopen(path.c_str(), "wb");
#endif
    if (g_file) {
        fputs("t,dur_us,frames,srate,lost\n", g_file);
        g_dir = dir;
    }
}

// Main thread, about 30 times a second.
void on_timer() {
    // Follow the recorder: open host.csv when it starts, close it when it stops or moves on.
    const char *want = GetExtState(SECTION, "host_trace_dir");
    if (!want) want = "";
    if (g_dir != want) {
        if (*want) open_file(want); else close_file();
    }
    // Drain everything published so far. Without a file the records are just discarded, so the
    // ring never fills up between recordings.
    const uint64_t head = g_head.load(std::memory_order_acquire);
    uint64_t tail = g_tail.load(std::memory_order_relaxed);
    const uint64_t lost = g_lost.load(std::memory_order_relaxed);
    for (; tail != head; ++tail) {
        const Block &b = g_ring[tail % CAP];
        if (g_file)
            fprintf(g_file, "%.9f,%.3f,%d,%.0f,%llu\n", b.t, b.dur * 1e6, b.frames, b.srate, (unsigned long long)lost);
#ifdef _WIN32
        if (g_shm) publish(b);                   // the HUD sees every block, recording or not
#endif
    }
#ifdef _WIN32
    if (g_shm) {
        g_shm[6] = static_cast<double>(lost);    // dropped blocks, for the HUD's "lost" count
        g_shm[7] = time_precise();               // REAPER's clock ...
        g_shm[5] = qpc_seconds();                // ... and QPC right after: the pair gives the offset; also the heartbeat
    }
#endif
    g_tail.store(tail, std::memory_order_release);   // hand the slots back to the audio thread
    const double now = time_precise();
    if (g_file && now - g_last_flush >= 1.0) {       // to disk once a second
        fflush(g_file);
        g_last_flush = now;
    }
}

// Look up one REAPER API function by name; false if this REAPER does not have it.
template <typename T> bool load(reaper_plugin_info_t *rec, T &fn, const char *name) {
    fn = reinterpret_cast<T>(rec->GetFunc(name));
    return fn != nullptr;
}

}  // namespace

// REAPER calls this once when it loads the DLL (rec set) and once when it unloads it (rec null).
// Returning 0 tells REAPER the extension did not start.
extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE, reaper_plugin_info_t *rec) {
    if (!rec) {   // unload: stop the hook before anything it uses goes away
        if (Audio_RegHardwareHook) Audio_RegHardwareHook(false, &g_hook);
        if (Register) Register("-timer", reinterpret_cast<void *>(on_timer));
        close_file();
        return 0;
    }
    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;   // built against a different API
    Register = rec->Register;
    if (!load(rec, time_precise, "time_precise") || !load(rec, Audio_RegHardwareHook, "Audio_RegHardwareHook") ||
        !load(rec, GetExtState, "GetExtState") || !load(rec, SetExtState, "SetExtState")) {
        return 0;
    }
    if (Audio_RegHardwareHook(true, &g_hook) <= 0) return 0;
#ifdef _WIN32
    open_shm();                                  // before the timer starts publishing into it
#endif
    Register("timer", reinterpret_cast<void *>(on_timer));
    SetExtState(SECTION, "host_ext", VERSION, false);
    return 1;
}
