// =================================================================================================
// cpu_clock_card.cpp - module "clock": the CPU's speed, and whether Windows is holding it back
// =================================================================================================
//
// Why it matters: a processor changes its speed all the time. Slowing down when there's little
// work is normal ("power saving") and harmless. But when the system CAPS the speed (too hot, on
// battery, a power-saving plan), every audio block takes longer, and a session that played fine
// starts to crackle. The "limit" line shows that cap: 100 % = no cap.
//
//   big number   the current average speed of the cores
//   rated        the speed the processor is sold as (its "base clock")
//   limit        how much of full speed the system allows (red below 99 %)
//   state        "full speed", "power saving" (slower, but not capped) or "throttled" (capped)
//
// Where the numbers come from: CpuClock (engine/src/cpu_clock.h), which reads Windows' "Processor
// Information" performance counters, the same ones Task Manager's speed figure uses. Read once a
// second. On macOS there's no public way to read the speed, so the module shows dashes.
// The idea of checking for throttling came up when discussing useful modules for a session.

#include <string>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class CpuClockCard final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "clock"; }
    const char *title() const override { return "CPU clock"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        const CpuClock::Reading clock = samples.empty() ? CpuClock::Reading{} : samples.back().clock;
        const bool slow = clock.mhz && clock.rated_mhz && *clock.mhz < *clock.rated_mhz * 0.9;   // under 90 % of rated
        const bool capped = clock.limit && *clock.limit < 0.99;                                    // the system limits it
        std::string state = DASH;
        if (clock.mhz) state = capped ? "throttled" : slow ? "power saving" : "full speed";
        const ImU32 warn_if_capped = capped ? colour::WARN : colour::TEXT;   // only a cap is trouble
        draw_card(draw, box, frame.scale, "CPU clock", ghz(clock.mhz), warn_if_capped,
                  {{"rated", ghz(clock.rated_mhz)},
                   {"limit", percent(clock.limit), warn_if_capped},
                   {"state", state, warn_if_capped}});
    }

protected:
    int lines() const override { return 3; }
};

}  // namespace

std::unique_ptr<Module> make_cpu_clock_card(const Fonts &fonts) { return std::make_unique<CpuClockCard>(fonts); }

}  // namespace sessionscope
