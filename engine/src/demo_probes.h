#ifndef SESSIONSCOPE_DEMO_PROBES_H
#define SESSIONSCOPE_DEMO_PROBES_H

// =================================================================================================
// demo_probes.h - made-up probes for the demo, so the Task Manager has something to show
// =================================================================================================
//
// The demo (--demo) has no DAW and no real probes. These pretend to be probes on a small song's
// tracks (drums, bass, lead, vocals) and write made-up timings into a private probe table through
// the real probes::Writer, so the Task Manager shows exactly what it would with real probes. The
// numbers rise when the generated song chokes. Generated data, not a measurement, like the rest
// of the demo.

#include <memory>
#include <string>
#include <vector>

#include "probe_table.h"

namespace sessionscope {

class DemoProbes {
public:
    DemoProbes();
    // The private table's name, for the ProbeMonitor that reads it.
    const std::string &table_name() const { return table_name_; }
    // Writes the blocks of the last `seconds` (ten times a second: 0.1). `strain` 0..1 makes every
    // plug-in work harder (the demo passes how late the song's blocks run).
    void tick(double seconds, double strain);

private:
    struct Fake {
        std::unique_ptr<probes::Writer> writer;
        double base;   // usual share of the block time
    };
    std::string table_name_;
    std::vector<Fake> fakes_;
    std::uint32_t random_ = 12345;   // a simple random number generator's state
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_DEMO_PROBES_H
