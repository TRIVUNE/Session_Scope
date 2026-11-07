// =================================================================================================
// cpu_number.cpp - module "cpu": the DAW's CPU as a big number
// =================================================================================================
//
// The DAW's share of the whole computer, averaged over the last second (single readings jump
// around too much to read), in large thin letters (Inter Display Light).
//
// Where the number comes from: the same readings as the peak meter (see peak_meter.cpp).
// This is the smallest module: a good one to copy when making a new one.

#include <algorithm>
#include <cmath>
#include <string>

#include "modules/draw_helpers.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

class CpuNumber final : public Module {
public:
    explicit CpuNumber(const Fonts &fonts) : fonts_(fonts) {}
    const char *id() const override { return "cpu"; }
    const char *title() const override { return "DAW CPU"; }

    // Across: as wide as the widest number, "100%". Down: as tall as the number.
    float fixed_length(bool vertical, float thickness, float scale) const override {
        const float size = text_size(thickness, scale);
        if (vertical) return std::round(size * 1.25f);
        return std::round(text_width(fonts_.number, size, "100%") + 6 * scale);
    }

    void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) override {
        // In a vertical strip the box is wide and short; size the text from the box's width then.
        const bool vertical = box.h < box.w * 0.6f;
        const float size = text_size(vertical ? box.w : box.h, frame.scale);
        const std::optional<double> share = daw_average(frame.system, frame.now, 1.0);
        const std::string text = percent(share);
        // Centred top to bottom; the small shift up looks centred to the eye (digits have no
        // descenders, so true centring looks low).
        const float y = box.y + (box.h - size) * 0.5f - size * 0.04f;
        draw->AddText(fonts_.number, size, at(box.x + 2 * frame.scale, y), share ? colour::TEXT : colour::TEXT_QUIET,
                      text.c_str());
        draw->AddText(fonts_.text, fonts_.text_size, at(box.x + 3 * frame.scale, box.y), colour::TEXT_QUIET, "DAW CPU");
    }

private:
    // The number's height: 62 % of the strip's thickness, at most 72 design pixels.
    static float text_size(float thickness, float scale) { return std::min(thickness * 0.62f, 72 * scale); }
    const Fonts &fonts_;
};

}  // namespace

std::unique_ptr<Module> make_cpu_number(const Fonts &fonts) { return std::make_unique<CpuNumber>(fonts); }

}  // namespace sessionscope
