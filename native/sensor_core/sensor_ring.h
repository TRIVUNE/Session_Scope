#ifndef SESSIONSCOPE_SENSOR_RING_H
#define SESSIONSCOPE_SENSOR_RING_H

// The sensor core: a cross-platform, real-time-safe ring in shared memory. Every plug-in format
// (VST3, CLAP, Audio Unit) is a thin wrapper around RingWriter.
//
// Every audio block the sensor writes one fixed-size record (time, block size, sample rate,
// transport state, position, tempo, time signature, its own cost) into a ring in a named
// shared-memory mapping. The separate SessionScope HUD app reads that ring. If no HUD is running,
// nothing reads it and nothing waits.
//
// The ring layout is the JSFX sensor's (hosts/reaper/Effects/SessionScope) plus one slot, so the
// analyser treats both alike (layout 2; layout 1 had no slot 12):
//   header  0 magic  1 layout  2 records written (published last)  3 capacity  4 slots per record
//           5 owner id  6 owner heartbeat  7 other id  8 other heartbeat  9 sensor version
//           10 process id of the host that owns the ring (0 if unknown; older sensors leave it 0)
//   record  0 t (seconds, host's high-res clock)  1 frames  2 srate  3 play_state  4 play_position (s)
//           5 position in quarter notes  6 tempo  7 ts_num  8 ts_denom  9 sensor cost of the previous
//           block (us)  10 sequence number  11 instance id  12 bar start in quarter notes
//           (VST3 barPositionMusic / CLAP equivalent / AU equivalent; NaN if the host does not give it)
// All slots are doubles. Only one instance writes; a second one stays silent and announces itself
// so the HUD can warn. Ownership passes on once the owner has been quiet for HANDOVER seconds.
//
// Real-time rules: write() allocates nothing, takes no locks, does no I/O, calls no host API.
// The mapping is created in the constructor (off the audio thread), never on the audio thread.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#endif

namespace sessionscope {

constexpr double MAGIC = 53530001;
constexpr double LAYOUT = 2;            // 2: slot 12 (bar start) added; the name below changed with it
constexpr double VERSION = 0.3;
constexpr int HDR = 16;                 // header slots before the first record
constexpr int REC = 13;                 // slots per record
constexpr uint64_t CAP = 32768;         // records in the ring: about 3 minutes at 48 kHz / 256
constexpr double HANDOVER = 0.25;       // seconds of owner silence before another instance takes over
#ifdef _WIN32
constexpr const char *MAPPING_NAME = "SessionScope_v2";
#else
constexpr const char *MAPPING_NAME = "/SessionScope_v2";
#endif
constexpr size_t MAPPING_BYTES = (HDR + CAP * REC) * sizeof(double);

// Seconds on a monotonic clock that the HUD reads too, so it can tell a live sensor from stale
// data by comparing the heartbeat with "now".
inline double now_seconds() {
#ifdef _WIN32
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
#elif defined(__APPLE__)
    return static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

// Current process ID as a double, for the header's process id slot (slot 10).
inline double process_id() {
#ifdef _WIN32
    return static_cast<double>(GetCurrentProcessId());
#else
    return static_cast<double>(getpid());
#endif
}

// The shared mapping, created on first use and kept for the life of the process (every plug-in
// instance in the host shares it). Never closed: the OS frees it when the host exits.
inline volatile double *shared_ring() {
    static volatile double *base = [] () -> volatile double * {
#ifdef _WIN32
        HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(MAPPING_BYTES), MAPPING_NAME);
        if (!h) return nullptr;
        void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, MAPPING_BYTES);
        return static_cast<volatile double *>(p);
#else
        int fd = shm_open(MAPPING_NAME, O_CREAT | O_RDWR, 0600);
        if (fd < 0) return nullptr;
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return nullptr;
        }
        // Size it only once: macOS refuses a second ftruncate.
        if (static_cast<size_t>(st.st_size) < MAPPING_BYTES) {
            if (ftruncate(fd, static_cast<off_t>(MAPPING_BYTES)) != 0) {
                close(fd);
                return nullptr;
            }
        }
        void *p = mmap(nullptr, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) return nullptr;
        return static_cast<volatile double *>(p);
#endif
    }();
    return base;
}

struct BlockInfo {
    double t0;          // block timestamp from now_seconds()
    double frames;      // number of samples in this block
    double srate;       // sample rate
    double play_state;  // 0=stopped, 1=playing, 5=recording
    double play_pos;    // play position in seconds (projectTimeSamples / srate)
    double qn;          // position in quarter notes (NaN if unknown)
    double tempo;       // tempo in BPM (NaN if unknown)
    double ts_num;      // time signature numerator (default 4)
    double ts_den;      // time signature denominator (default 4)
    double bar_qn;      // bar start in quarter notes (NaN if unknown)
};

class RingWriter {
public:
    // `self` is the owning plug-in instance. The id mixes the clock with its address: good enough
    // to tell instances apart.
    explicit RingWriter(const void *self) {
        id_ = 1.0 + std::floor(std::fmod(now_seconds() * 1e6, 900000.0)) + (reinterpret_cast<uintptr_t>(self) >> 4) % 99999;
        ring_ = shared_ring();   // create the mapping now, off the audio thread
    }

    // Write one record to the ring. On the audio thread: allocates nothing, takes no locks,
    // does no I/O, calls no host API.
    void write(const BlockInfo &b) {
        if (!ring_) return;
        if (b.frames <= 0) return;   // zero-sample calls (e.g., VST3 parameter flush) write nothing

        const double t0 = b.t0;
        const double owner = ring_[5];
        const bool mine = owner == id_;

        // Another instance is currently writing and hasn't been quiet for HANDOVER seconds.
        if (!(mine || owner == 0 || t0 - ring_[6] > HANDOVER)) {
            ring_[7] = id_;
            ring_[8] = t0;
            return;
        }

        // Take over: claim the ring, and set up the header if it is missing or different.
        if (!mine) {
            ring_[5] = id_;
            if (ring_[0] != MAGIC || ring_[1] != LAYOUT ||
                ring_[3] != static_cast<double>(CAP) || ring_[4] != REC) {
                ring_[2] = 0;
                ring_[1] = LAYOUT;
                ring_[3] = static_cast<double>(CAP);
                ring_[4] = REC;
                ring_[0] = MAGIC;   // last: the header is valid only once this is set
            }
            // Every take-over: the new owner may be a different DAW or a newer sensor build.
            ring_[9] = VERSION;
            ring_[10] = process_id();
        }

        const uint64_t n = static_cast<uint64_t>(ring_[2]);
        volatile double *p = ring_ + HDR + (n % CAP) * REC;

        p[0] = b.t0;
        p[1] = b.frames;
        p[2] = b.srate;
        p[3] = b.play_state;
        p[4] = b.play_pos;
        p[5] = b.qn;
        p[6] = b.tempo;
        p[7] = b.ts_num;
        p[8] = b.ts_den;
        p[9] = prev_cost_;
        p[10] = static_cast<double>(n);
        p[11] = id_;
        p[12] = b.bar_qn;

        // Publish: fields before the count, then the count, then the heartbeat.
        std::atomic_thread_fence(std::memory_order_release);
        ring_[2] = static_cast<double>(n + 1);
        ring_[6] = t0;
    }

    // Call after process() to report this block's cost (in microseconds) for the *next* record.
    void set_cost(double us) { prev_cost_ = us; }

    double id() const { return id_; }

private:
    double id_ = 0;
    double prev_cost_ = 0;
    volatile double *ring_ = nullptr;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_SENSOR_RING_H
