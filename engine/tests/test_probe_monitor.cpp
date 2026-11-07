// Tests for the probe table (native/sensor_core/probe_table.h) and ProbeMonitor: probes write
// through the real Writer into a private table, the monitor reads CPU per plug-in and per track.

#include "probe_monitor.h"
#include "probe_table.h"
#include "test_support.h"

#include <string>

using namespace sessionscope;

int main() {
    // A table name of our own, so the test never mixes with real probes or another test run.
    const std::string name = std::string(probes::TABLE_NAME) + "_test_" + std::to_string(probes::now_ns());

    int a = 0, b = 0, c = 0;   // three "probe instances" (their addresses make the ids)
    probes::Writer synth(&a, name.c_str()), reverb(&b, name.c_str()), drums(&c, name.c_str());
    CHECK(synth.claim() && reverb.claim() && drums.claim());
    synth.set_names("Lead", "Serum", 2, 0);
    reverb.set_names("Lead", "Valhalla", 2, 0);
    drums.set_names("Drums", "Kontakt", 1, 0);

    ProbeMonitor monitor(name.c_str());
    monitor.tick();   // first reading: the starting point
    // One block of 5.333 ms (256 samples at 48 kHz): synth 2 ms, reverb 0.5 ms, drums 1 ms, then
    // a second synth block that took 4 ms (its peak).
    const std::uint64_t block = 5'333'333;
    synth.record_block(2'000'000, block);
    synth.record_block(4'000'000, block);
    reverb.record_block(500'000, block);
    drums.record_block(1'000'000, block);
    monitor.tick();

    const auto &tracks = monitor.tracks();
    CHECK(tracks.size() == 2);
    CHECK(tracks[0].name == "Drums");   // DAW order: Drums (1) before Lead (2)
    CHECK(tracks[1].name == "Lead" && tracks[1].plugins.size() == 2);
    CHECK_NEAR(tracks[0].cpu, 1.0 / 5.333333, 0.001);
    const PluginLoad &serum = tracks[1].plugins[0].name == "Serum" ? tracks[1].plugins[0] : tracks[1].plugins[1];
    CHECK_NEAR(serum.cpu, 6.0 / (2 * 5.333333), 0.001);   // 6 ms over two blocks
    CHECK_NEAR(serum.peak, 0.75, 0.001);                  // the 4 ms block: 750 per mille
    CHECK_NEAR(tracks[1].cpu, 6.0 / (2 * 5.333333) + 0.5 / 5.333333, 0.001);   // a track = the sum of its plug-ins
    CHECK_NEAR(tracks[1].peak, 0.75 + 0.5 / 5.333333, 0.001);   // a track's peak = its plug-ins' peaks added up

    // A stopped probe disappears; a rename shows on the next reading.
    reverb.release();
    synth.set_names("Lead synth", "Serum", 2, 0);
    synth.record_block(1'000'000, block);
    monitor.tick();
    bool found_renamed = false, found_reverb = false;
    for (const TrackLoad &t : monitor.tracks()) {
        if (t.name == "Lead synth") found_renamed = true;
        for (const PluginLoad &p : t.plugins) found_reverb = found_reverb || p.name == "Valhalla";
    }
    CHECK(found_renamed);
    CHECK(!found_reverb);

    // Probes from another DAW are left out when the DAW's process is known.
    drums.set_names("Drums", "Kontakt", 1, 4242);
    monitor.tick(1111);
    for (const TrackLoad &t : monitor.tracks()) CHECK(t.name != "Drums");

    // Zero-sample blocks (VST3 parameter flushes) don't count.
    synth.record_block(123, 0);
    return finish_tests();
}
