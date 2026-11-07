#ifndef SESSIONSCOPE_HUD_THEME_H
#define SESSIONSCOPE_HUD_THEME_H

// The HUD's look, shared by every module: light and quiet, like a plain plug-in panel. A pale
// grey background, white panels with thin dark borders, light-grey fills, the heatmap in heat
// colours ("jet": dark blue, cyan, green, yellow, red), Inter for all text.

#include <imgui.h>

namespace sessionscope {

namespace colour {
constexpr ImU32 BACKGROUND = IM_COL32(0xf2, 0xf2, 0xf2, 0xff);   // behind the modules
constexpr ImU32 PANEL = IM_COL32(0xff, 0xff, 0xff, 0xff);        // inside a module's frame
constexpr ImU32 BORDER = IM_COL32(0x3c, 0x3c, 0x3c, 0xff);       // a module's frame: one pixel
constexpr ImU32 TEXT = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);
constexpr ImU32 TEXT_QUIET = IM_COL32(0x8c, 0x8c, 0x8c, 0xff);   // titles, labels, empty values
constexpr ImU32 WARN = IM_COL32(0xc8, 0x28, 0x1e, 0xff);         // a value that means trouble
constexpr ImU32 FILL = IM_COL32(0xc6, 0xc6, 0xc6, 0xff);         // a meter's level
constexpr ImU32 MARK = IM_COL32(0x00, 0x00, 0x00, 0xff);         // peak mark, bar lines
constexpr ImU32 HOVER = IM_COL32(0xe6, 0xe6, 0xe6, 0xff);        // a menu item under the mouse
constexpr ImU32 DRAG = IM_COL32(0x1e, 0x64, 0xc8, 0xff);         // a module being moved, where it lands
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
