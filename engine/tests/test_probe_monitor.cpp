// Tests for the probe table (native/sensor_core/probe_table.h) and ProbeMonitor: probes write
// through the real Writer into a private table, the monitor reads CPU per plug-in and per track.

#include "probe_monitor.h"
#include "probe_table.h"
#include "test_support.h"

#include <memory>
#include <string>
#include <vector>

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

    // ---- who may take a slot ---------------------------------------------------------------------
    // (reverb gave its slot back above; synth and drums hold theirs: 2 of the 256 are taken)
    probes::Table *table = probes::open_table(name.c_str());
    CHECK(table != nullptr);
    if (!table) return finish_tests();

    // A probe that has been silent for a long time (a bypassed plug-in, a project still loading)
    // keeps its slot. Make synth and drums look silent since the computer started:
    probes::Slot *free_slot = nullptr;
    for (probes::Slot &slot : table->slot) {
        if (slot.owner.load() != 0) slot.heartbeat_ns.store(0);
        else if (!free_slot) free_slot = &slot;
    }
    // A slot left behind by a program that no longer runs (process id 4294967280: no such program)
    // is free to take.
    CHECK(free_slot != nullptr);
    if (free_slot) free_slot->owner.store((std::uint64_t{0xFFFFFFF0u} << 32) | 5);
    CHECK(!probes::process_alive(0xFFFFFFF0u));
    CHECK(probes::process_alive(probes::this_process()));

    // Now take slots until the table is full: every slot but synth's and drums' must be handed
    // out, the dead program's among them, and never one twice.
    static int addresses[probes::SLOTS + 1];   // each writer needs an address of its own for its id
    std::vector<std::unique_ptr<probes::Writer>> writers;
    for (std::uint32_t i = 0; i <= probes::SLOTS; ++i) {
        auto writer = std::make_unique<probes::Writer>(&addresses[i], name.c_str());
        if (!writer->claim()) break;
        writers.push_back(std::move(writer));
    }
    CHECK(writers.size() == probes::SLOTS - 2);
    CHECK(free_slot && static_cast<std::uint32_t>(free_slot->owner.load() >> 32) == probes::this_process());   // the dead one was taken
    // synth and drums still write into slots that are theirs alone: their blocks keep counting from where they were.
    synth.set_names("Lead synth", "Serum", 2, 0);
    drums.set_names("Drums", "Kontakt", 1, 0);
    int serum_slots = 0, kontakt_slots = 0;
    for (const probes::Slot &slot : table->slot) {
        if (std::string(slot.plugin) == "Serum") ++serum_slots;
        if (std::string(slot.plugin) == "Kontakt") ++kontakt_slots;
    }
    CHECK(serum_slots == 1 && kontakt_slots == 1);
    // Giving slots back makes room again.
    writers.pop_back();
    int late = 0;
    probes::Writer latecomer(&late, name.c_str());
    CHECK(latecomer.claim());
    return finish_tests();
}
