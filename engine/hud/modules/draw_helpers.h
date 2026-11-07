#ifndef SESSIONSCOPE_HUD_DRAW_HELPERS_H
#define SESSIONSCOPE_HUD_DRAW_HELPERS_H

// =================================================================================================
// draw_helpers.h - small pieces every module uses
// =================================================================================================
//
//   panel()          a white box with a 1-pixel dark frame
//   percent(), ...   numbers turned into short text ("12%", "2.4 GB", "3.20 GHz")
//   jet_colour()     a load from 0 to 1 turned into a heatmap colour
//   LevelFollower    a value that rises fast, falls slowly, and keeps a peak mark (meters)
//   level_bar()      draws a LevelFollower as a bar
//   Card             the small modules' shared look: title, big value, a few label/value lines
//
// About text and speed: the strings these make are short (under 16 characters), and C++ keeps
// strings that short inside the string itself ("small string optimisation"), so making them every
// frame doesn't ask the system for memory.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "modules/module.h"

namespace sessionscope {

// ------------------------------------------------------------------------------------- boxes

// ImVec2 is Dear ImGui's (x, y) point; at(x, y) is just shorter to write.
inline ImVec2 at(float x, float y) { return ImVec2(x, y); }

// A module's frame: white inside, one dark pixel around.
void panel(ImDrawList *draw, const Box &box);

// The inside of a box, `inset` pixels in from each edge.
inline Box inside(const Box &b, float inset) { return {b.x + inset, b.y + inset, b.w - 2 * inset, b.h - 2 * inset}; }

// ------------------------------------------------------------------------------------- text

// Shown in place of a value we don't know (an en dash).
constexpr const char *DASH = "\xE2\x80\x93";

std::string percent(std::optional<double> share);   // 0.12 -> "12%", 0.034 -> "3.4%"
std::string bytes(std::optional<double> b);          // 2.4e9 -> "2.4 GB", 5e8 -> "500 MB"
std::string ghz(std::optional<double> mhz);          // 3200 -> "3.20 GHz"

// ------------------------------------------------------------------------------------- colour

// A load from 0 (idle) to 1 (full) as a heatmap colour, using the "jet" colour scale: dark blue,
// blue, cyan, green, yellow, orange, red. Jet is the classic colour scale of spectrum analysers
// and of MATLAB; the person picked it from a spectrogram screenshot.
ImU32 jet_colour(float load);

// ------------------------------------------------------------------------------------- readings

// The DAW's CPU, averaged over the last `seconds`; nothing if no reading had it.
std::optional<double> daw_average(const SystemMonitor &system, double now, double seconds);

// The busiest core in one reading: its number and how busy it was (0..1); nothing if no cores.
std::optional<std::pair<int, float>> busiest_core(const SystemSample &sample);

// ------------------------------------------------------------------------------------- meters

// A level for a meter. It follows a target value, but:
//   - rises quickly (in about 40 ms) so peaks show at once,
//   - falls slowly (in about 450 ms) so the eye can follow it,
//   - keeps a "peak mark" at the highest value for 2 seconds, then lets it sink.
// This is how audio level meters behave ("fast attack, slow release, peak hold").
class LevelFollower {
public:
    // Call once per frame with the newest value and the time now (seconds).
    void follow(double target, double now);
    float level() const;   // 0..1, what the bar shows
    float peak() const;    // 0..1, where the peak mark is

private:
    double level_ = 0;
    double peak_ = 0;
    double peak_time_ = -1e9;   // when the peak mark was last raised
    double last_ = -1;          // the time of the previous follow(); -1 before the first
};

// Draws `level` as a bar with its peak mark inside a framed box. Fills upward when the box is
// taller than wide, rightward when it's wider.
void level_bar(ImDrawList *draw, const Box &box, const LevelFollower &level, float scale);

// ------------------------------------------------------------------------------------- cards

// One "label ........ value" line of a card.
struct CardLine {
    std::string label;
    std::string value;
    ImU32 colour = colour::TEXT;
};

// The look shared by the small modules (busiest core, dropouts, memory, clock): a frame with a
// quiet title at the top, one big value, then a few label/value lines. A module based on Card
// says how many lines it has and calls draw_card() from its draw().
class Card : public Module {
public:
    explicit Card(const Fonts &fonts) : fonts_(fonts) {}
    float fixed_length(bool vertical, float thickness, float scale) const override;

protected:
    virtual int lines() const = 0;   // how many label/value lines, for sizing in a vertical strip
    float pad(float scale) const { return 8 * scale; }           // space inside the frame
    float value_size(float scale) const { return 26 * scale; }   // height of the big value's text

    // Draws the frame, title, big value and lines. Returns the box left over below them (a module
    // can draw more there, like the busiest core's little bar).
    Box draw_card(ImDrawList *draw, const Box &box, float scale, const char *title, const std::string &value,
                  ImU32 value_colour, const std::vector<CardLine> &lines) const;

    const Fonts &fonts_;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_DRAW_HELPERS_H
