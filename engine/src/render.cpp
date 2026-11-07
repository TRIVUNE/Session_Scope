// Console rendering; render.h describes what it draws.

#include "render.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "text_format.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sessionscope {

namespace {

// Checks if stdout is a TTY.
bool is_stdout_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// Checks if NO_COLOR environment variable is set (no-color.org).
bool no_color_env() {
    const char *no_color = std::getenv("NO_COLOR");
    return no_color != nullptr && no_color[0] != '\0';
}

// Detects if the console uses UTF-8 encoding.
// On Windows, assumes true because main() sets the code page to UTF-8.
// On POSIX, checks LC_ALL, LC_CTYPE, LANG for "UTF" or "utf".
bool detect_unicode() {
#if defined(_WIN32)
    return true;
#else
    const char *env_vars[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (const char *var : env_vars) {
        const char *val = std::getenv(var);
        if (val) {
            std::string s(val);
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (s.find("utf") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
#endif
}

}  // namespace

std::array<int, 3> heat_rgb(double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    for (size_t i = 0; i < HEAT.size() - 1; ++i) {
        double t0 = HEAT[i].first;
        double t1 = HEAT[i + 1].first;
        if (t <= t1) {
            double f = (t1 == t0) ? 0.0 : (t - t0) / (t1 - t0);
            std::array<int, 3> result;
            for (int c = 0; c < 3; ++c) {
                double a = static_cast<double>(HEAT[i].second[c]);
                double b = static_cast<double>(HEAT[i + 1].second[c]);
                result[c] = static_cast<int>(std::nearbyint(a + (b - a) * f));
            }
            return result;
        }
    }
    return HEAT.back().second;
}

Style::Style(std::optional<bool> color, std::optional<bool> unicode) {
    if (color.has_value()) {
        color_ = color.value();
    } else {
        color_ = is_stdout_tty() && !no_color_env();
    }

    if (unicode.has_value()) {
        unicode_ = unicode.value();
    } else {
        unicode_ = detect_unicode();
    }

#if defined(_WIN32)
    if (color_) {
        // The Windows console prints colour codes as text unless told otherwise.
        const HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(console, &mode)) SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

const std::vector<std::string> &Style::levels() const {
    return unicode_ ? LEVELS : ASCII_LEVELS;
}

std::string Style::paint(std::string_view text,
                         const std::array<int, 3> *rgb,
                         bool dim,
                         bool bold) const {
    if (!color_) {
        return std::string(text);
    }

    std::vector<std::string> codes;
    if (bold) codes.push_back("1");
    if (dim) codes.push_back("2");
    if (rgb) {
        codes.push_back("38;2;" +
                        std::to_string((*rgb)[0]) + ";" +
                        std::to_string((*rgb)[1]) + ";" +
                        std::to_string((*rgb)[2]));
    }
    if (codes.empty()) {
        return std::string(text);
    }

    std::string result = "\x1b[";
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i > 0) result += ";";
        result += codes[i];
    }
    result += "m";
    result += text;
    result += "\x1b[0m";
    return result;
}

std::string Style::cell(const Metric &metric, const Cell *c) const {
    const std::vector<std::string> &level = levels();

    if (!c || (!c->value.has_value() && c->n == 0)) {
        return " ";  // nothing played in this bar
    }
    if (!c->value.has_value()) {
        return paint(unicode_ ? "·" : ".", nullptr, true);  // played, but too few blocks
    }

    const double f = std::min(1.0, std::max(0.0, c->value.value() / metric.scale));

    // Never the blank level (index 0): it played.
    const int index = std::max(1, static_cast<int>(std::nearbyint(f * static_cast<double>(level.size() - 1))));

    // An overrun must stand out without colour.
    const std::string character = !color_ && tier(metric, c->value.value()) >= 3 ? "!" : level[index];

    const std::array<int, 3> rgb = heat_rgb(f);
    return paint(character, &rgb);
}

std::string fmt(const Metric &metric, std::optional<double> v) {
    if (!v.has_value() || std::isnan(v.value())) {
        return "–";  // U+2013 en dash
    }
    double val = v.value();
    if (metric.unit.rfind("share", 0) == 0) {  // starts with "share"
        // Percentage with 0 decimals: f"{v:.0%}"
        return fixed(val * 100.0, 0) + "%";
    }
    if (metric.unit == "ms") {
        return fixed(val, 2) + " ms";
    }
    return fixed(val, 2);
}

namespace {

// Builds the ruler string for bars [start, end].
// Bar numbers at the first bar and every 8th (1, 9, 17, ...), one column per bar.
std::string build_ruler(int start, int end) {
    std::string out;
    out.reserve(end - start + 1);
    int b = start;
    while (b <= end) {
        if (b == start || (b - 1) % 8 == 0) {
            std::string s = std::to_string(b);
            out += s;
            b += static_cast<int>(s.size());
        } else {
            out += " ";
            ++b;
        }
    }
    // Trim to exact width
    if (static_cast<int>(out.size()) > end - start + 1) {
        out.resize(end - start + 1);
    }
    return out;
}

}  // namespace

std::vector<std::string> heatmap(const Style &style,
                                 const Metric &metric,
                                 const std::vector<std::pair<std::string, std::map<int, Cell>>> &rows,
                                 int first,
                                 std::optional<int> last,
                                 int label_w) {
    // Collect all bar numbers
    std::vector<int> all_bars;
    for (const auto &row : rows) {
        for (const auto &kv : row.second) {
            all_bars.push_back(kv.first);
        }
    }
    if (all_bars.empty()) {
        return {"(no playing bars)"};
    }

    int last_bar = last.value_or(*std::max_element(all_bars.begin(), all_bars.end()));
    first = std::min(first, *std::min_element(all_bars.begin(), all_bars.end()));

    std::vector<std::string> lines;
    for (int start = first; start <= last_bar; start += BARS_PER_LINE) {
        int end = std::min(last_bar, start + BARS_PER_LINE - 1);

        // Ruler line
        lines.push_back(padded_right("", label_w) + style.paint(build_ruler(start, end), nullptr, true));

        // Data rows
        for (const auto &row : rows) {
            std::string label = row.first;
            if (static_cast<int>(label.size()) > label_w - 1) {
                label.resize(label_w - 1);
            }
            label = padded_right(label, label_w);

            std::string bar_chars;
            bar_chars.reserve(end - start + 1);
            for (int b = start; b <= end; ++b) {
                auto it = row.second.find(b);
                const Cell *cell = (it != row.second.end()) ? &it->second : nullptr;
                bar_chars += style.cell(metric, cell);
            }
            lines.push_back(label + bar_chars);
        }
    }
    return lines;
}

std::string legend(const Style &style, const Metric &metric) {
    std::vector<std::string> parts;
    const std::vector<std::string> &level = style.levels();

    for (int i = 0; i < 4; ++i) {
        const double lo = (i == 0) ? 0.0 : metric.tiers[i - 1];
        const double hi = (i < 3) ? metric.tiers[i] : lo * 1.1;
        const double f = std::min(1.0, (lo + hi) / 2.0 / metric.scale);   // the middle of the tier's range

        const int index = std::max(1, static_cast<int>(std::nearbyint(f * static_cast<double>(level.size() - 1))));
        const std::array<int, 3> rgb = heat_rgb(f);
        std::string part = style.paint(level[index], &rgb) + " " + TIER_NAMES[i];
        if (i > 0) {
            part += (style.unicode() ? " ≥" : " >=") + fmt(metric, lo);
        }
        parts.push_back(part);
    }

    // "too few blocks" indicator
    std::string few = style.paint(style.unicode() ? "·" : ".", nullptr, true) + " too few blocks";
    parts.push_back(few);

    // Join with two spaces
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "  ";
        result += parts[i];
    }
    return result;
}

}  // namespace sessionscope
