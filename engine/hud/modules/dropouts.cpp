// =================================================================================================
// dropouts.cpp - module "dropouts": audio blocks that came close to (or past) their deadline
// =================================================================================================
//
// Some background: the DAW makes sound in small blocks (for example 256 samples = 5.3 ms at 48 kHz)
// and each block must be ready before the sound card needs it, its "deadline". A block that's late
// is heard as a click or dropout. The sensor plug-in times every block; the engine works out how
// late each one ran ("lateness", as a share of the block's length).
//
// This module shows:
//   big number   how many blocks were late enough to count as an overrun since the HUD started
//   headroom     how much room the slowest blocks of the last second still have before that line
//                (100 % = plenty, 0 % = at the line)
//   last         when and in which bar of the song the last overrun happened
//   late p99     the lateness that 99 % of the last second's blocks stayed under
//
// Where the numbers come from: the engine (engine/src/hud_engine.h, Snapshot::overruns and
// late.p99), which reads the sensor plug-in's records. "Overrun" is the lateness metric's highest
// tier (Metric::tiers[2], engine/src/stats.h).

#include <algorithm>
#include <cstdio>
#include <string>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class Dropouts final : public Card {
public:
    using Card::Card;
    const char *id() const override { return "dropouts"; }
    const char *title() const override { return "Dropouts"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const Snapshot &s = frame.snapshot;
        if (!s.ready) {   // the sensor isn't playing yet
            draw_card(draw, box, frame.scale, "Dropouts", DASH, colour::TEXT_QUIET, {{"sensor", "not playing"}});
            return;
        }
        const double overrun = s.late_tiers[2];   // the lateness that counts as an overrun
        std::optional<double> headroom;
        if (s.late.p99) headroom = std::clamp(1.0 - *s.late.p99 / overrun, 0.0, 1.0);

        std::string last = DASH;
        if (!s.recent_overruns.empty()) {
            const Overrun &o = s.recent_overruns.back();   // newest last
            char text[48];
            if (o.bar) std::snprintf(text, sizeof text, "bar %d, %.0f s ago", *o.bar, o.ago_s);
            else std::snprintf(text, sizeof text, "%.0f s ago", o.ago_s);
            last = text;
        }
        draw_card(draw, box, frame.scale, "Dropouts", std::to_string(s.overruns), s.overruns ? colour::WARN : colour::TEXT,
                  {{"headroom", percent(headroom), headroom && *headroom < 0.25 ? colour::WARN : colour::TEXT},
                   {"last", last},
                   {"late p99", percent(s.late.p99)}});
    }

protected:
    int lines() const override { return 3; }
};

}  // namespace

std::unique_ptr<Module> make_dropouts(const Fonts &fonts) { return std::make_unique<Dropouts>(fonts); }

}  // namespace sessionscope
