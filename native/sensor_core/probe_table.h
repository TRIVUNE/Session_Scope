#ifndef SESSIONSCOPE_PROBE_TABLE_H
#define SESSIONSCOPE_PROBE_TABLE_H

// =================================================================================================
// probe_table.h - where every SessionScope Probe reports how much CPU its plug-in uses
// =================================================================================================
//
// A SessionScope Probe is a plug-in that loads a real plug-in inside itself and times it (see
// handoff/task-manager-plan.md). Every probe in the DAW writes into one shared table in memory; the
// HUD reads it ten times a second for its Task Manager module.
//
// Shared memory: a block of memory the operating system lets several programs open by name, here
// "SessionScope_probes_v1". The DAW (where the probes run) and the HUD both open it.
//
// The table: a header and SLOTS slots. A probe takes a free slot when it starts and frees it when
// it stops. Each slot holds:
//   owner          which probe holds the slot (0 = free): the DAW's process id in the upper half
//                  of the number, the probe's own number in the lower half
//   heartbeat_ns   when it last wrote (the HUD stops showing a probe that was silent for STALE_NS)
//   blocks         how many audio blocks it has timed
//   cost_ns        total time spent inside the real plug-in
//   budget_ns      total real-time length of those blocks (frames / sample rate)
//   peak_permille  the worst single block since the HUD last looked: cost / budget x 1000
//   names          track name, plug-in name, track position, DAW process id
//
// The HUD works out CPU % by comparing two readings: (cost difference) / (budget difference).
//
// Rules for the audio thread (record_block): no locks, no memory allocation, no system calls
// except reading the clock. The counters are std::atomic, which the processor updates in one
// step, so the HUD can read them at any moment without stopping the probe. The names change
// rarely and are guarded by a "sequence counter": the writer makes it odd while writing and even
// when done; a reader that sees an odd or changed counter just tries again later.
//
// When is a slot free to take? When its owner is 0, or when the program that owned it no longer
// runs (a DAW that crashed never freed its slots). NOT when its probe has merely been silent: a
// bypassed plug-in, or one in a project that takes long to load, is silent and still alive. (An
// earlier version handed out slots after five silent seconds; in a real REAPER session two probes
// then wrote into one slot. Found in the first live test, November 2026.)
//
// Header-only: the probe plug-ins and the engine both include this file.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sessionscope {
namespace probes {

constexpr std::uint64_t MAGIC = 0x53535052'4f424531ull;   // "SSPROBE1"
constexpr std::uint32_t LAYOUT = 1;
constexpr std::uint32_t SLOTS = 256;                       // more probes than any session needs
constexpr std::size_t NAME_BYTES = 64;                     // names are cut to 63 characters
constexpr std::uint64_t STALE_NS = 5'000'000'000ull;       // 5 s without writing: the HUD stops showing the probe
#ifdef _WIN32
constexpr const char *TABLE_NAME = "SessionScope_probes_v1";
#else
constexpr const char *TABLE_NAME = "/SessionScope_probes_v1";
#endif

// Shared memory needs atomics that work across programs: true for 64-bit numbers on every
// processor we build for. This line stops the build if that ever isn't so.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "probe table needs lock-free 64-bit atomics");

struct Slot {
    std::atomic<std::uint64_t> owner;
    std::atomic<std::uint64_t> heartbeat_ns;
    std::atomic<std::uint64_t> blocks;
    std::atomic<std::uint64_t> cost_ns;
    std::atomic<std::uint64_t> budget_ns;
    std::atomic<std::uint64_t> peak_permille;
    std::atomic<std::uint64_t> names_version;   // even = names readable, odd = being written
    std::int64_t track_order;                   // the track's position in the DAW, -1 if unknown
    std::uint64_t process_id;                   // the DAW's process
    char track[NAME_BYTES];
    char plugin[NAME_BYTES];
};

struct Table {
    std::atomic<std::uint64_t> magic;   // MAGIC once the table is set up
    std::uint32_t layout;
    std::uint32_t slots;
    Slot slot[SLOTS];
};

// Nanoseconds on a steady clock every program on the computer shares (QueryPerformanceCounter on
// Windows, the monotonic clock elsewhere): heartbeats from the DAW and the HUD's "now" compare.
inline std::uint64_t now_ns() {
#ifdef _WIN32
    static const double to_ns = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e9 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(static_cast<double>(c.QuadPart) * to_ns);
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Opens (and, the first time, creates) the shared table called `name`. Returns nullptr if the
// system won't. The memory stays mapped for the life of the program. `name` other than
// TABLE_NAME is for tests and the demo, so they never mix with real probes.
inline Table *open_table(const char *name = TABLE_NAME) {
    const std::size_t bytes = sizeof(Table);
    void *memory = nullptr;
#ifdef _WIN32
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), name);
    if (!mapping) return nullptr;
    memory = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    // The handle stays open on purpose: closing the last handle would delete the table.
#else
    const int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    struct stat st;
    if (fstat(fd, &st) != 0 || (static_cast<std::size_t>(st.st_size) < bytes && ftruncate(fd, static_cast<off_t>(bytes)) != 0)) {
        close(fd);
        return nullptr;
    }
    memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) memory = nullptr;
#endif
    if (!memory) return nullptr;
    auto *table = static_cast<Table *>(memory);
    // New memory is all zeros, which is a valid empty table (every atomic starts at 0). Mark it.
    if (table->magic.load() != MAGIC) {
        table->layout = LAYOUT;
        table->slots = SLOTS;
        table->magic.store(MAGIC);
    }
    return table;
}

// This program's process id (the number the system gives every running program).
inline std::uint32_t this_process() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

// Is the program with this process id still running? When the system won't let us look, the
// answer is yes: better to leave a slot alone than to take one that is in use.
inline bool process_alive(std::uint32_t process_id) {
    if (process_id == this_process()) return true;
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;   // "no such process" is the other answer
    DWORD exit_code = 0;
    const bool running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return kill(static_cast<pid_t>(process_id), 0) == 0 || errno == EPERM;   // signal 0: only asks, sends nothing
#endif
}

// Copies a name into a slot field, cut to fit, always ending in a zero byte.
inline void copy_name(char (&field)[NAME_BYTES], const char *text) {
    std::size_t n = 0;
    while (text && text[n] && n + 1 < NAME_BYTES) {
        field[n] = text[n];
        ++n;
    }
    field[n] = '\0';
}

// The probe's side. One per probe plug-in instance.
class Writer {
public:
    // `self`: the probe instance. Opens the table; call off the audio thread (the plug-in's
    // constructor or setup).
    explicit Writer(const void *self, const char *table_name = TABLE_NAME)
        : id_(make_id(self)), table_(open_table(table_name)) {}
    ~Writer() { release(); }
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    // Takes a free slot: one nobody owns, or one whose program no longer runs. Off the audio
    // thread (it asks the system about other programs). False if the table is full or couldn't
    // be opened; the probe then simply doesn't report.
    bool claim() {
        if (!table_ || slot_) return slot_ != nullptr;
        const std::uint64_t now = now_ns();
        for (Slot &s : table_->slot) {
            std::uint64_t owner = s.owner.load();
            // Free: no owner; or our own id (left by an earlier program that had our process id
            // and crashed); or an owner whose program is gone. The process id is the upper half.
            const bool free = owner == 0 || owner == id_ || !process_alive(static_cast<std::uint32_t>(owner >> 32));
            if (free && s.owner.compare_exchange_strong(owner, id_)) {   // only one probe can win a slot
                s.heartbeat_ns.store(now);
                s.blocks.store(0);
                s.cost_ns.store(0);
                s.budget_ns.store(0);
                s.peak_permille.store(0);
                slot_ = &s;
                return true;
            }
        }
        return false;
    }

    // Sets the names the HUD shows. Off the audio thread; call again when the host renames the track.
    void set_names(const char *track, const char *plugin, std::int64_t track_order, std::uint64_t process_id) {
        if (!slot_) return;
        slot_->names_version.fetch_add(1);   // odd: readers wait
        copy_name(slot_->track, track);
        copy_name(slot_->plugin, plugin);
        slot_->track_order = track_order;
        slot_->process_id = process_id;
        slot_->names_version.fetch_add(1);   // even: done
    }

    // One audio block: `cost_ns` spent inside the real plug-in, `budget_ns` the block's real-time
    // length. On the audio thread: only atomic adds and stores.
    void record_block(std::uint64_t cost_ns, std::uint64_t budget_ns) {
        if (!slot_ || budget_ns == 0) return;   // 0-sample blocks (parameter flushes) don't count
        slot_->blocks.fetch_add(1, std::memory_order_relaxed);
        slot_->cost_ns.fetch_add(cost_ns, std::memory_order_relaxed);
        slot_->budget_ns.fetch_add(budget_ns, std::memory_order_relaxed);
        // Raise the peak if this block is worse; the HUD sets it back to 0 when it reads it.
        const std::uint64_t permille = cost_ns * 1000 / budget_ns;
        std::uint64_t peak = slot_->peak_permille.load(std::memory_order_relaxed);
        while (permille > peak && !slot_->peak_permille.compare_exchange_weak(peak, permille, std::memory_order_relaxed)) {
        }
        slot_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    }

    // Frees the slot. Off the audio thread (the plug-in's destructor).
    void release() {
        if (slot_) {
            std::uint64_t mine = id_;
            slot_->owner.compare_exchange_strong(mine, 0);   // free it only if it is (still) ours
        }
        slot_ = nullptr;
    }

    bool active() const { return slot_ != nullptr; }

private:
    // The id that marks a slot as ours: this program's process id in the upper 32 bits (so anyone
    // can tell whether the owner still runs), and in the lower 32 bits a number from the probe's
    // address, which no other probe alive in this program shares. Never 0 (0 = free).
    static std::uint64_t make_id(const void *self) {
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(self));
        const auto low = static_cast<std::uint32_t>(address ^ (address >> 32));   // both halves of the address, folded together
        return (static_cast<std::uint64_t>(this_process()) << 32) | (low ? low : 1u);
    }

    std::uint64_t id_;
    Table *table_;
    Slot *slot_ = nullptr;
};

}  // namespace probes
}  // namespace sessionscope

#endif  // SESSIONSCOPE_PROBE_TABLE_H
