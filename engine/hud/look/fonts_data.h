#ifndef SESSIONSCOPE_HUD_FONTS_DATA_H
#define SESSIONSCOPE_HUD_FONTS_DATA_H

// =================================================================================================
// fonts_data.h - the HUD's fonts, built into the program
// =================================================================================================
//
// The fonts are not files next to the .exe: their bytes are compiled into it, so the HUD always has
// them. How that happens:
//   1. tools/get_hud_deps.py downloads Inter 4.1 (github.com/rsms/inter, SIL Open Font Licence),
//      checks the download's SHA-256 fingerprint, and keeps two font files in .toolchain/inter.
//   2. tools/build_engine.py turns each file into a long list of numbers (its bytes) in a generated
//      C++ file, build/engine/obj/hud_generated/fonts_data.cpp, which defines the two names below.
//   3. hud_view.cpp hands those bytes to Dear ImGui, which draws the letters.

namespace sessionscope {

// A font file's bytes, as compiled into the program.
struct EmbeddedFont {
    const unsigned char *data;   // the first byte
    int size;                    // how many bytes
};

extern const EmbeddedFont INTER_DISPLAY_LIGHT;   // thin letters for big numbers
extern const EmbeddedFont INTER_REGULAR;         // normal letters for small text

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_FONTS_DATA_H
