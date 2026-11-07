// =================================================================================================
// demo_probes.cpp - made-up probes for the demo (see demo_probes.h)
// =================================================================================================

#include "demo_probes.h"

#include <cmath>

namespace sessionscope {

namespace {

// The made-up session: track, plug-in, its usual share of the block time.
struct FakeSpec {
    const char *track;
    const char *plugin;
    double base;
};
constexpr FakeSpec SESSION[] = {
    {"Drums", "Kontakt 7", 0.06},   {"Drums", "Drum Bus Compressor", 0.015},
    {"Bass", "Serum", 0.08},        {"Bass", "Pro-Q 3", 0.01},
    {"Lead", "Serum", 0.11},        {"Lead", "ValhallaRoom", 0.04},
    {"Vocals", "Melodyne", 0.05},   {"Vocals", "De-esser", 0.008}, {"Vocals", "ValhallaVintageVerb", 0.035},
};

constexpr double BLOCK_S = 256.0 / 48000.0;   // 256 samples at 48 kHz: 5.33 ms

}  // namespace

DemoProbes::DemoProbes() : table_name_(std::string(probes::TABLE_NAME) + "_demo_" + std::to_string(probes::now_ns())) {
    std::int64_t order = 0;
    const char *last_track = "";
    for (const FakeSpec &spec : SESSION) {
        if (std::string(spec.track) != last_track) ++order;   // a new track: the next position
        last_track = spec.track;
        Fake fake{std::make_unique<probes::Writer>(&spec, table_name_.c_str()), spec.base};
        fake.writer->claim();
        fake.writer->set_names(spec.track, spec.plugin, order, 0);
        fakes_.push_back(std::move(fake));
    }
}

void DemoProbes::tick(double seconds, double strain) {
    const int blocks = static_cast<int>(std::lround(seconds / BLOCK_S));
    const auto budget = static_cast<std::uint64_t>(BLOCK_S * 1e9);
    for (Fake &fake : fakes_) {
        for (int b = 0; b < blocks; ++b) {
            // A little randomness per block, and now and then a block twice as slow.
            random_ = random_ * 1664525u + 1013904223u;   // the classic "LCG" random number formula
            const double jitter = 0.8 + 0.4 * static_cast<double>(random_ >> 8) / 16777216.0;
            const double spike = (random_ & 0xff) < 3 ? 2.0 : 1.0;
            const double share = fake.base * (1.0 + 2.5 * strain) * jitter * spike;
            fake.writer->record_block(static_cast<std::uint64_t>(share * static_cast<double>(budget)), budget);
        }
    }
}

}  // namespace sessionscope
