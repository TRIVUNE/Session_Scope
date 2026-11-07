// =================================================================================================
// busiest_core.cpp - module "busiest": the single busiest core
// =================================================================================================
//
// Why it matters: audio is usually computed on one thread, which runs on one core at a time. That
// core can be full (and the audio crackle) while the computer's overall CPU looks low, because the
// other cores are idle. This module shows the busiest core right now, which one it is, and the
// highest any core reached in the last two seconds.
//
// Where the numbers come from: the per-core readings (CoreCpu, engine/src/core_cpu.h).

#include <string>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class BusiestCore final : public Card {
public:
    using Card::Card;   // use Card's constructor
    const char *id() const override { return "busiest"; }
    const char *title() const override { return "Busiest core"; }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        const auto &samples = frame.system.samples();
        const auto top = samples.empty() ? std::nullopt : busiest_core(samples.back());
        level_.follow(top ? top->second : 0.0, frame.now);

        // The busiest core of the last two seconds: walk back from the newest reading.
        std::optional<std::pair<int, float>> recent;
        for (auto it = samples.rbegin(); it != samples.rend() && it->t >= frame.now - 2.0; ++it) {
            const auto t = busiest_core(*it);
            if (t && (!recent || t->second > recent->second)) recent = t;
        }

        const std::optional<double> top_share = top ? std::optional<double>(top->second) : std::nullopt;
        const std::string peak = recent ? "core " + std::to_string(recent->first) + "  " + percent(recent->second) : DASH;
        const Box rest = draw_card(draw, box, frame.scale, "Busiest core", percent(top_share),
                                   top && top->second >= 0.9f ? colour::WARN : colour::TEXT,   // red from 90 %
                                   {{"core now", top ? std::to_string(top->first) : DASH}, {"peak, 2 s", peak}});

        // Under the lines, if there's room: a small level bar of the busiest core.
        const float p = pad(frame.scale);
        if (rest.h > 10 * frame.scale) {
            level_bar(draw, {rest.x + p, rest.y, rest.w - 2 * p, std::min(rest.h - p, 10 * frame.scale)}, level_, frame.scale);
        }
    }

protected:
    int lines() const override { return 3; }   // two text lines and the little bar

private:
    LevelFollower level_;
};

}  // namespace

std::unique_ptr<Module> make_busiest_core(const Fonts &fonts) { return std::make_unique<BusiestCore>(fonts); }

}  // namespace sessionscope
