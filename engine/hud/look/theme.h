#ifndef SESSIONSCOPE_HUD_THEME_H
#define SESSIONSCOPE_HUD_THEME_H

// =================================================================================================
// theme.h - the HUD's colours and fonts, in one place
// =================================================================================================
//
// Every module draws with these, so changing a colour here changes it everywhere.
//
// The look: a plain, light plug-in panel. Pale grey behind everything, white boxes with a thin dark
// border, grey text for labels, dark text for values, red only for values that mean trouble.
//
// Where it comes from:
//   - The light panel look follows a reference screenshot the person gave (a "Spectral
//     Compressor" plug-in window): white boxes, thin dark borders, grey fills.
//   - The font is Inter 4.1 by Rasmus Andersson (SIL Open Font Licence), see fonts_data.h.
//
// A colour is written IM_COL32(red, green, blue, alpha), each 0..255 (0x00..0xff in hexadecimal).
// Alpha is how opaque it is: 0xff (255) = solid.

#include <imgui.h>   // Dear ImGui: ImU32 (a packed colour), ImFont

namespace sessionscope {

namespace colour {
constexpr ImU32 BACKGROUND = IM_COL32(0xf2, 0xf2, 0xf2, 0xff);   // pale grey, behind the modules
constexpr ImU32 PANEL = IM_COL32(0xff, 0xff, 0xff, 0xff);        // white, inside a module's box
constexpr ImU32 BORDER = IM_COL32(0x3c, 0x3c, 0x3c, 0xff);       // dark grey, a box's 1-pixel frame
constexpr ImU32 TEXT = IM_COL32(0x3a, 0x3a, 0x3a, 0xff);         // values
constexpr ImU32 TEXT_QUIET = IM_COL32(0x8c, 0x8c, 0x8c, 0xff);   // titles, labels, unknown values
constexpr ImU32 WARN = IM_COL32(0xc8, 0x28, 0x1e, 0xff);         // red: a value that means trouble
constexpr ImU32 FILL = IM_COL32(0xc6, 0xc6, 0xc6, 0xff);         // light grey: a meter's level, lines
constexpr ImU32 MARK = IM_COL32(0x00, 0x00, 0x00, 0xff);         // black: peak marks, bar lines
constexpr ImU32 HOVER = IM_COL32(0xe6, 0xe6, 0xe6, 0xff);        // a menu item under the mouse
constexpr ImU32 DRAG = IM_COL32(0x1e, 0x64, 0xc8, 0xff);         // blue: a module being moved
}  // namespace colour

// The background again, as four numbers from 0 to 1 (red, green, blue, alpha): the graphics card
// wants it this way when it clears the window before each frame.
constexpr float BACKGROUND_RGBA[4] = {0xf2 / 255.0f, 0xf2 / 255.0f, 0xf2 / 255.0f, 1.0f};

// The two fonts the HUD uses, loaded once at start (hud_view.cpp).
struct Fonts {
    ImFont *number = nullptr;   // Inter Display Light, made large: big numbers like "12%"
    ImFont *text = nullptr;     // Inter Regular: all small text
    float text_size = 12;       // the small text's height, in window pixels
};

// How wide `text` is when drawn with `font` at `size`, in pixels. Used to right-align numbers.
inline float text_width(ImFont *font, float size, const char *text) {
    return font->CalcTextSizeA(size, 1e9f, 0, text).x;
}

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_THEME_H
