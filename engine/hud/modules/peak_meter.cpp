// =================================================================================================
// peak_meter.cpp - module "meter": the DAW's CPU as a bar with a peak mark
// =================================================================================================
//
// Shows how much of the whole computer the DAW is using right now, the way an audio level meter
// shows loudness: the bar jumps up fast, falls back slowly, and a black mark stays at the highest
// point for two seconds (LevelFollower in draw_helpers.h does that).
//
// Where the number comes from: SystemMonitor reads the DAW process's CPU time ten times a second
// (ProcessCpu, engine/src/process_cpu.h, the same measure as Task Manager's CPU column). The sensor
// plug-in tells the HUD which process is the DAW.
//
// Where the idea comes from: the peak meters of MiniMeters and of every DAW mixer.

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class PeakMeter final : public Module {
public:
    const char *id() const override { return "meter"; }
    const char *title() const override { return "Peak meter (DAW CPU)"; }
    // A thin bar: 18 design pixels across, whatever the strip's direction.
    float fixed_length(bool, float, float scale) const override { return 18 * scale; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        // The newest reading, or 0 when there is none yet (no DAW found).
        const double now_value = !samples.empty() && samples.back().daw ? *samples.back().daw : 0.0;
        level_.follow(now_value, frame.now);
        level_bar(draw, box, level_, frame.scale);
    }

private:
    LevelFollower level_;
};

}  // namespace

std::unique_ptr<Module> make_peak_meter() { return std::make_unique<PeakMeter>(); }

}  // namespace sessionscope
