#ifndef SESSIONSCOPE_HUD_FONTS_DATA_H
#define SESSIONSCOPE_HUD_FONTS_DATA_H

// The HUD's fonts, compiled into the program (tools/build_engine.py writes the bytes from
// .toolchain/inter, fetched by tools/get_hud_deps.py). Inter 4.1, SIL Open Font Licence.

namespace sessionscope {

struct EmbeddedFont {
    const unsigned char *data;
    int size;
};

extern const EmbeddedFont INTER_DISPLAY_LIGHT;   // the big CPU number
extern const EmbeddedFont INTER_REGULAR;         // small text: bar numbers

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_FONTS_DATA_H
