#ifndef SESSIONSCOPE_RENDER_H
#define SESSIONSCOPE_RENDER_H

// Console rendering: the HUD prototype's blue-to-red scale as 24-bit ANSI colour, or plain text.
//
// Everything the terminal shows is built here: one character per bar for heatmaps, a ruler with bar
// numbers above them, and the legend underneath. Nothing in this file computes statistics.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "stats.h"

namespace sessionscope {

// Colour stops of the HUD prototype (design/hud-prototype/index.html, STOPS.heat):
// (position on the 0..1 scale, (red, green, blue)). Blue when light, red when heavy.
inline const std::array<std::pair<double, std::array<int, 3>>, 7> HEAT = {{
    {0.0,   {29,  63, 191}},
    {0.3,   {47, 123, 255}},
    {0.5,   {95, 208, 255}},
    {0.65,  {184, 242, 201}},
    {0.78,  {255, 224, 138}},
    {0.9,   {255, 154,  74}},
    {1.0,   {255,  59,  48}}
}};

// Bar heights from empty to full, one string per level: a Unicode block takes several bytes, so the
// levels cannot be indexed as one string. The ASCII set is for terminals without Unicode.
inline const std::vector<std::string> LEVELS = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
inline const std::vector<std::string> ASCII_LEVELS = {" ", ".", ":", "-", "=", "+", "*", "#", "%", "@"};

// Heatmaps wrap after this many bars so they fit an ordinary terminal.
inline constexpr int BARS_PER_LINE = 64;

// Returns the RGB colour for a position t on the 0..1 scale, blended between
// the two nearest stops. Clamps t to [0, 1].
// Returns {r, g, b} with each component in 0..255.
std::array<int, 3> heat_rgb(double t);

// What the terminal can show: colour or not, Unicode or ASCII. Detected unless given.
class Style {
public:
    // color: if true, emit ANSI colour codes. If nullopt, auto-detect:
    //   true only on a real terminal (isatty(stdout)) and never when NO_COLOR is set.
    // unicode: if true, use Unicode block elements. If nullopt, auto-detect:
    //   on Windows, true (main() sets code page to UTF-8); on POSIX, from
    //   LC_ALL, LC_CTYPE, or LANG containing "UTF" or "utf".
    explicit Style(std::optional<bool> color = std::nullopt,
                   std::optional<bool> unicode = std::nullopt);

    // Returns the level characters for this style.
    const std::vector<std::string> &levels() const;

    // Wraps text in ANSI codes (bold, dim, 24-bit colour), or returns it untouched
    // without colour. If rgb is provided, uses 24-bit foreground colour.
    std::string paint(std::string_view text,
                      const std::array<int, 3> *rgb = nullptr,
                      bool dim = false,
                      bool bold = false) const;

    // One character for a bar: height and colour both show the value, as in the prototype.
    // metric: the metric being displayed (for scale and tiers)
    // c: the cell (may be empty, or have value == nullopt)
    std::string cell(const Metric &metric, const Cell *c) const;

    bool color() const { return color_; }
    bool unicode() const { return unicode_; }

private:
    bool color_;
    bool unicode_;
};

// Formats a value in the metric's unit: percentages for shares, milliseconds for times.
// Returns "–" (en dash) for NaN or missing values.
std::string fmt(const Metric &metric, std::optional<double> v);

// Builds a heatmap from rows of (label, {bar -> Cell}).
// rows: vector of (label, map<bar, Cell>)
// first: first bar number to show (default 1)
// last: last bar number to show (default: max bar in any row)
// label_w: width reserved for labels (default 10)
// Returns vector of text lines: a ruler, then one line per row, wrapped every BARS_PER_LINE bars.
std::vector<std::string> heatmap(const Style &style,
                                 const Metric &metric,
                                 const std::vector<std::pair<std::string, std::map<int, Cell>>> &rows,
                                 int first = 1,
                                 std::optional<int> last = std::nullopt,
                                 int label_w = 10);

// Builds the legend line: one sample character per tier with its threshold.
// Returns a single string with parts joined by "  " (two spaces).
std::string legend(const Style &style, const Metric &metric);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_RENDER_H
