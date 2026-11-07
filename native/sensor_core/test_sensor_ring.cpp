// Test for sensor_ring.h - plain main(), no framework.
// Returns non-zero and prints the failure on any failed check.
// Prints "ok" at the end if all checks pass.
// Uses the real shared ring, so do not run it while a DAW has the sensor loaded.

#include "sensor_ring.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace sessionscope;

void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    // Zero header slots 0-15 of the ring so a previous run does not interfere.
    volatile double *g = shared_ring();
    if (!g) {
        std::fprintf(stderr, "FAIL: shared_ring() returned nullptr\n");
        return 1;
    }
    for (int i = 0; i < 16; ++i) {
        g[i] = 0;
    }

    // Two RingWriters A and B (distinct pointers) on the shared ring.
    int dummyA = 0, dummyB = 0;
    RingWriter A(&dummyA);
    RingWriter B(&dummyB);

    // A.write with frames 0 publishes nothing (g[2] unchanged, g[5] unchanged).
    BlockInfo bi0{};
    bi0.t0 = now_seconds();
    bi0.frames = 0;
    bi0.srate = 48000;
    bi0.play_state = 1;
    bi0.play_pos = 0;
    bi0.qn = 0;
    bi0.tempo = 120;
    bi0.ts_num = 4;
    bi0.ts_den = 4;
    bi0.bar_qn = 0;
    A.write(bi0);
    check(g[2] == 0, "frames=0 should not publish (count unchanged)");
    check(g[5] == 0, "frames=0 should not claim ownership");

    // A.write with frames 256 sets up header and writes record.
    BlockInfo bi1 = bi0;
    bi1.frames = 256;
    bi1.t0 = now_seconds();
    bi1.qn = 1.5;
    bi1.bar_qn = 0;
    A.write(bi1);

    check(g[0] == MAGIC, "g[0] == MAGIC");
    check(g[1] == LAYOUT, "g[1] == LAYOUT");
    check(g[3] == static_cast<double>(CAP), "g[3] == CAP");
    check(g[4] == REC, "g[4] == REC");
    check(g[9] == VERSION, "g[9] == VERSION");
    check(g[10] == process_id(), "g[10] == process id");
    check(g[2] == 1, "count raises by 1");

    // Check the record slot holds the expected values.
    volatile double *p = g + HDR + 0 * REC;
    check(p[0] == bi1.t0, "slot 0: t0");
    check(p[1] == bi1.frames, "slot 1: frames");
    check(p[2] == bi1.srate, "slot 2: srate");
    check(p[3] == bi1.play_state, "slot 3: play_state");
    check(p[4] == bi1.play_pos, "slot 4: play_pos");
    check(p[5] == bi1.qn, "slot 5: qn");
    check(p[6] == bi1.tempo, "slot 6: tempo");
    check(p[7] == bi1.ts_num, "slot 7: ts_num");
    check(p[8] == bi1.ts_den, "slot 8: ts_den");
    check(p[9] == 0, "slot 9: prev_cost (0 for first write)");
    check(p[10] == 0, "slot 10: sequence number (0 for first record)");
    check(p[11] == A.id(), "slot 11: instance id");
    check(p[12] == bi1.bar_qn, "slot 12: bar_qn");

    // B.write immediately after is silent: count unchanged, g[7] == B's id (not 0 and not g[5]).
    BlockInfo bi2 = bi1;
    bi2.t0 = now_seconds();
    B.write(bi2);
    check(g[2] == 1, "B silent: count unchanged");
    check(g[7] != 0 && g[7] != g[5], "B announces itself: g[7] == B's id (not 0, not owner)");

    // B.write with t0 = A's last t0 + 0.3 (past HANDOVER) takes over and restamps the owner.
    g[9] = 0;
    g[10] = 0;
    BlockInfo bi3 = bi1;
    bi3.t0 = bi1.t0 + 0.3;  // > HANDOVER (0.25)
    bi3.frames = 256;
    bi3.qn = 2.0;
    bi3.bar_qn = 0;
    B.write(bi3);
    check(g[5] != A.id(), "B takes over: g[5] changes (not A's id)");
    check(g[5] == B.id(), "B takes over: g[5] == B's id");
    check(g[2] == 2, "B takes over: count raises to 2");
    check(g[9] == VERSION, "take-over: version written even though the header was valid");
    check(g[10] == process_id(), "take-over: process id written even though the header was valid");

    // Verify B's record
    volatile double *p2 = g + HDR + 1 * REC;
    check(p2[10] == 1, "B's record: sequence number 1");
    check(p2[11] == B.id(), "B's record: instance id is B's");

    std::printf("ok\n");
    return 0;
}
