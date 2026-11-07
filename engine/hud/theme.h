#ifndef SESSIONSCOPE_HUD_THEME_H
#define SESSIONSCOPE_HUD_THEME_H

// The HUD's look, shared by every module: light and quiet, like a plain plug-in panel. A pale
// grey background, white panels with thin dark borders, light-grey fills, the heatmap in heat
// colours (green, yellow, red), Inter for all text.

#include <imgui.h>

namespace sessionscope {

namespace colour {
constexpr ImU32 BACKGROUND = IM_COL32(0xf2, 0xf2, 0xf2, 0xff);   // behind the modules
constexpr ImU32 PANEL = IM_COL32(0xff, 0xff, 0xff, 0xff);        // inside a module's frame
constexpr ImU32 BORDER = IM_COL32(0x3c, 0x3c, 0x3c, 0xff);       // a module's frame: one pixel
constexpr ImU32 TEXT = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);
constexpr ImU32 TEXT_QUIET = IM_COL32(0x8c, 0x8c, 0x8c, 0xff);   // column titles, empty values
constexpr ImU32 FILL = IM_COL32(0xc6, 0xc6, 0xc6, 0xff);         // the meter's level
constexpr ImU32 MARK = IM_COL32(0x00, 0x00, 0x00, 0xff);         // peak mark, bar lines
constexpr ImU32 ROW_NOW = IM_COL32(0xe6, 0xe6, 0xe6, 0xff);      // the bar playing now
constexpr ImU32 AT_RISK = IM_COL32(0xf8, 0xdc, 0x8c, 0xff);      // a bar list cell past "at risk"
constexpr ImU32 OVERRUN = IM_COL32(0xf3, 0xa0, 0x98, 0xff);      // ... past "overrun"
}  // namespace colour

// The same background, for clearing the swap chain before ImGui draws.
constexpr float BACKGROUND_RGBA[4] = {0xf2 / 255.0f, 0xf2 / 255.0f, 0xf2 / 255.0f, 1.0f};

struct Fonts {
    ImFont *number = nullptr;   // Inter Display Light, rasterised large: the CPU number
    ImFont *text = nullptr;     // Inter Regular: everything small
    float text_size = 12;       // window units
};

inline float text_width(ImFont *font, float size, const char *text) {
    return font->CalcTextSizeA(size, 1e9f, 0, text).x;
}

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_THEME_H
