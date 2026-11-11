// Tests for render.h and text_format.h: number formatting, heat colours and the pieces a console
// report is drawn from.

#include "render.h"
#include "text_format.h"
#include "test_support.h"

#include <array>
#include <map>
#include <string>
#include <vector>

using namespace sessionscope;

int main() {
    // ----- text_format tests -----
    {
        // fixed: matches Python's f"{value:.2f}" with round-half-even
        CHECK(fixed(1.234, 2) == "1.23");
        CHECK(fixed(1.235, 2) == "1.24");   // round half to even
        CHECK(fixed(1.236, 2) == "1.24");
        CHECK(fixed(2.675, 2) == "2.67");   // 2.675 rounds to 2.67 (half to even)
        CHECK(fixed(-1.235, 2) == "-1.24");
        CHECK(fixed(0.0, 2) == "0.00");
        CHECK(fixed(100.0, 2) == "100.00");
        CHECK(fixed(1234567.89, 2) == "1234567.89");
        CHECK(fixed(std::numeric_limits<double>::quiet_NaN(), 2) == "nan");
        CHECK(fixed(std::numeric_limits<double>::infinity(), 2) == "inf");
        CHECK(fixed(-std::numeric_limits<double>::infinity(), 2) == "-inf");

        // general: matches Python's f"{value:g}"
        CHECK(general(1.23) == "1.23");
        CHECK(general(123.456) == "123.456");
        CHECK(general(1234567.89) == "1.23457e+06");
        CHECK(general(0.000123) == "0.000123");
        CHECK(general(0.0) == "0");
        CHECK(general(std::numeric_limits<double>::quiet_NaN()) == "nan");
        CHECK(general(std::numeric_limits<double>::infinity()) == "inf");
        CHECK(general(-std::numeric_limits<double>::infinity()) == "-inf");

        // with_commas: matches Python's f"{value:,}"
        CHECK(with_commas(0) == "0");
        CHECK(with_commas(1) == "1");
        CHECK(with_commas(12) == "12");
        CHECK(with_commas(123) == "123");
        CHECK(with_commas(1234) == "1,234");
        CHECK(with_commas(12345) == "12,345");
        CHECK(with_commas(123456) == "123,456");
        CHECK(with_commas(1234567) == "1,234,567");
        CHECK(with_commas(-12345) == "-12,345");
        CHECK(with_commas(-1234567) == "-1,234,567");

        // display_length: counts Unicode code points
        CHECK(display_length("hello") == 5);
        CHECK(display_length("hi") == 2);
        CHECK(display_length("hello world") == 11);
        CHECK(display_length("café") == 4);       // e + acute = 2 code points
        CHECK(display_length("日本語") == 3);
        CHECK(display_length("a\u2582b") == 3);   // middle dot
        CHECK(display_length("") == 0);

        // padded_left/right: width in code points
        CHECK(padded_left("hello", 10) == "     hello");
        CHECK(padded_right("hello", 10) == "hello     ");
        CHECK(padded_left("hi", 10) == "        hi");
        CHECK(padded_right("hi", 10) == "hi        ");
        CHECK(padded_left("hello world", 10) == "hello world");  // already wider
        CHECK(padded_right("hello world", 10) == "hello world");
        CHECK(padded_left("café", 10) == "      café");
        CHECK(padded_right("café", 10) == "café      ");
        CHECK(padded_left("日本語", 10) == "       日本語");
        CHECK(padded_right("日本語", 10) == "日本語       ");
        CHECK(padded_left("a\u2582b", 10) == "       a\u2582b");
        CHECK(padded_right("a\u2582b", 10) == "a\u2582b       ");

        // py_round: round half to even
        CHECK(py_round(0.5) == 0.0);
        CHECK(py_round(1.5) == 2.0);
        CHECK(py_round(2.5) == 2.0);
        CHECK(py_round(3.5) == 4.0);
        CHECK(py_round(-0.5) == -0.0);
        CHECK(py_round(-1.5) == -2.0);
        CHECK(py_round(-2.5) == -2.0);
        CHECK(py_round(2.675) == 3.0);
    }

    // ----- heat_rgb tests (values computed from Python) -----
    {
        // At stops
        CHECK((heat_rgb(0.0) == std::array<int, 3>{29,  63, 191}));
        CHECK((heat_rgb(0.3) == std::array<int, 3>{47, 123, 255}));
        CHECK((heat_rgb(0.5) == std::array<int, 3>{95, 208, 255}));
        CHECK((heat_rgb(0.65) == std::array<int, 3>{184, 242, 201}));
        CHECK((heat_rgb(0.78) == std::array<int, 3>{255, 224, 138}));
        CHECK((heat_rgb(0.9) == std::array<int, 3>{255, 154,  74}));
        CHECK((heat_rgb(1.0) == std::array<int, 3>{255,  59,  48}));

        // Between stops
        CHECK((heat_rgb(0.15) == std::array<int, 3>{38,  93, 223}));
        CHECK((heat_rgb(0.4)  == std::array<int, 3>{71, 166, 255}));
        CHECK((heat_rgb(0.575) == std::array<int, 3>{139, 225, 228}));
        CHECK((heat_rgb(0.715) == std::array<int, 3>{219, 233, 170}));
        CHECK((heat_rgb(0.84)  == std::array<int, 3>{255, 189, 106}));
        CHECK((heat_rgb(0.95)  == std::array<int, 3>{255, 107,  61}));

        // Clamping
        CHECK((heat_rgb(-0.5) == heat_rgb(0.0)));
        CHECK((heat_rgb(1.5) == heat_rgb(1.0)));
    }

    // ----- Style tests -----
    {
        Style style_color_unicode(true, true);
        CHECK(style_color_unicode.color() == true);
        CHECK(style_color_unicode.unicode() == true);
        CHECK(style_color_unicode.levels() == LEVELS);

        Style style_plain(false, false);
        CHECK(style_plain.color() == false);
        CHECK(style_plain.unicode() == false);
        CHECK(style_plain.levels() == ASCII_LEVELS);

        // paint without color
        CHECK(style_plain.paint("test") == "test");
        CHECK(style_plain.paint("test", nullptr, true) == "test");
        CHECK(style_plain.paint("test", nullptr, false, true) == "test");

        // paint with color (ANSI sequences)
        std::array<int, 3> rgb{255, 0, 0};
        std::string colored = style_color_unicode.paint("X", &rgb);
        CHECK(colored.find("\x1b[38;2;255;0;0mX\x1b[0m") != std::string::npos);
        std::string colored_dim = style_color_unicode.paint("X", &rgb, true);
        CHECK(colored_dim.find("\x1b[2;38;2;255;0;0mX\x1b[0m") != std::string::npos);
        std::string colored_bold = style_color_unicode.paint("X", &rgb, false, true);
        CHECK(colored_bold.find("\x1b[1;38;2;255;0;0mX\x1b[0m") != std::string::npos);
        std::string colored_both = style_color_unicode.paint("X", &rgb, true, true);
        CHECK(colored_both.find("\x1b[1;2;38;2;255;0;0mX\x1b[0m") != std::string::npos ||
              colored_both.find("\x1b[2;1;38;2;255;0;0mX\x1b[0m") != std::string::npos);
    }

    // ----- cell tests -----
    {
        Style style_c(true, true);
        Style style_p(false, false);
        const Metric &m = metric_by_key("lateness");  // scale=0.45, tiers=(0.10, 0.20, 0.40)

        // None cell (no blocks)
        Cell c_none;
        c_none.value = std::nullopt;
        c_none.n = 0;
        CHECK(style_c.cell(m, &c_none) == " ");
        CHECK(style_p.cell(m, &c_none) == " ");

        // Few blocks (value = nullopt, n > 0)
        Cell c_few;
        c_few.value = std::nullopt;
        c_few.n = 10;
        CHECK(style_c.cell(m, &c_few) == "\x1b[2m\xc2\xb7\x1b[0m");  // dim middle dot
        // Plain style has no color, so paint returns text untouched even with dim
        CHECK(style_p.cell(m, &c_few) == ".");

        // Value at various levels
        Cell c1; c1.value = 0.01; c1.n = 10;  // f = 0.01/0.45 = 0.022, level 1
        Cell c2; c2.value = 0.09; c2.n = 10;  // f = 0.2, level ~2
        Cell c3; c3.value = 0.15; c3.n = 10;  // f = 0.33, level ~3
        Cell c4; c4.value = 0.45; c4.n = 10;  // f = 1.0, level 8 (█)
        Cell c5; c5.value = 0.90; c5.n = 10;  // f > 1, clamped to 1.0, level 8

        // Just verify they return non-empty strings with color
        std::string s1 = style_c.cell(m, &c1);
        CHECK(s1.size() > 1);  // includes ANSI codes
        std::string s4 = style_c.cell(m, &c4);
        CHECK(s4.size() > 1);

        // Plain style: no color, use ASCII levels
        std::string p1 = style_p.cell(m, &c1);
        CHECK(p1.size() == 1);
        CHECK(p1 != " ");  // not blank

        // Overrun without color shows '!'
        Cell c_over; c_over.value = 0.5; c_over.n = 10;  // tier 3 (overrun)
        CHECK(style_p.cell(m, &c_over) == "!");
    }

    // ----- fmt tests -----
    {
        const Metric &late = metric_by_key("lateness");      // unit="share of a block"
        const Metric &work = metric_by_key("workload");       // unit="share of the deadline"
        const Metric &late_ms = metric_by_key("lateness_ms"); // unit="ms"
        const Metric &interval = metric_by_key("interval");   // unit="blocks"
        const Metric &host_rt = metric_by_key("host_rt");     // unit="share of the deadline"

        // share units -> percentage with 0 decimals
        CHECK(fmt(late, 0.1) == "10%");
        CHECK(fmt(late, 0.5) == "50%");
        CHECK(fmt(late, 1.0) == "100%");
        CHECK(fmt(work, 0.1) == "10%");
        CHECK(fmt(host_rt, 0.5) == "50%");

        // ms unit
        CHECK(fmt(late_ms, 0.1) == "0.10 ms");
        CHECK(fmt(late_ms, 0.5) == "0.50 ms");
        CHECK(fmt(late_ms, 1.0) == "1.00 ms");

        // other units -> fixed 2 decimals
        CHECK(fmt(interval, 0.1) == "0.10");
        CHECK(fmt(interval, 0.5) == "0.50");
        CHECK(fmt(interval, 1.0) == "1.00");

        // NaN/None -> en dash
        CHECK(fmt(late, std::nullopt) == "\u2013");
        CHECK(fmt(late, std::numeric_limits<double>::quiet_NaN()) == "\u2013");
    }

    // ----- _ruler tests (internal, but we can test via heatmap) -----
    // The ruler is tested implicitly via heatmap tests below.

    // ----- heatmap tests -----
    {
        const Metric &m = metric_by_key("lateness");
        std::vector<std::pair<std::string, std::map<int, Cell>>> rows = {
            {"Pass 1", {{1, Cell{0.1, 10}}, {2, Cell{0.2, 10}}, {3, Cell{0.3, 10}}, {9, Cell{0.5, 10}}}},
            {"Pass 2", {{1, Cell{0.15, 10}}, {2, Cell{0.25, 10}}, {3, Cell{0.35, 10}}, {9, Cell{0.6, 10}}}}
        };

        // Color + Unicode
        Style style_c(true, true);
        auto lines_c = heatmap(style_c, m, rows, 1, 9, 10);
        CHECK(lines_c.size() == 3);  // 1 ruler + 2 rows
        // Ruler line
        CHECK(lines_c[0] == "          \x1b[2m1       9\x1b[0m");
        // Data lines - check they contain expected ANSI sequences
        CHECK(lines_c[1].find("\x1b[38;2;42;107;238m") != std::string::npos);  // bar 1
        CHECK(lines_c[1].find("\x1b[38;2;82;184;255m") != std::string::npos);  // bar 2
        CHECK(lines_c[1].find("\x1b[38;2;193;240;193m") != std::string::npos); // bar 3
        CHECK(lines_c[1].find("\x1b[38;2;255;59;48m") != std::string::npos);   // bar 9 (clamped)
        CHECK(lines_c[2].find("\x1b[38;2;55;137;255m") != std::string::npos);  // bar 1
        CHECK(lines_c[2].find("\x1b[38;2;128;221;235m") != std::string::npos); // bar 2
        CHECK(lines_c[2].find("\x1b[38;2;254;224;139m") != std::string::npos); // bar 3
        CHECK(lines_c[2].find("\x1b[38;2;255;59;48m") != std::string::npos);   // bar 9

        // Plain (no color, ASCII)
        Style style_p(false, false);
        auto lines_p = heatmap(style_p, m, rows, 1, 9, 10);
        CHECK(lines_p.size() == 3);
        CHECK(lines_p[0] == "          1       9");
        CHECK(lines_p[1] == "Pass 1    :=*     !");
        CHECK(lines_p[2] == "Pass 2    -+#     !");

        // Empty rows
        auto empty = heatmap(style_p, m, {}, 1, std::nullopt, 10);
        CHECK(empty.size() == 1);
        CHECK(empty[0] == "(no playing bars)");
    }

    // ----- legend tests -----
    {
        const Metric &m = metric_by_key("lateness");  // tiers (0.10, 0.20, 0.40), scale 0.45

        // Color + Unicode
        Style style_c(true, true);
        std::string leg_c = legend(style_c, m);
        // Expected from Python:
        // '\x1b[38;2;36;85;215m\u2581\x1b[0m normal  \x1b[38;2;55;137;255m\u2583\x1b[0m elevated \u226510%  \x1b[38;2;193;240;193m\u2585\x1b[0m at risk \u226520%  \x1b[38;2;255;122;65m\u2587\x1b[0m overrun \u226540%  \x1b[2m\xb7\x1b[0m too few blocks'
        CHECK(leg_c.find("\x1b[38;2;36;85;215m") != std::string::npos);   // normal
        CHECK(leg_c.find("\x1b[38;2;55;137;255m") != std::string::npos);   // elevated
        CHECK(leg_c.find("\x1b[38;2;193;240;193m") != std::string::npos);  // at risk
        CHECK(leg_c.find("\x1b[38;2;255;122;65m") != std::string::npos);   // overrun
        CHECK(leg_c.find("\x1b[2m\xc2\xb7\x1b[0m") != std::string::npos);  // too few blocks (dim middle dot)
        CHECK(leg_c.find("normal") != std::string::npos);
        CHECK(leg_c.find("elevated \u226510%") != std::string::npos);
        CHECK(leg_c.find("at risk \u226520%") != std::string::npos);
        CHECK(leg_c.find("overrun \u226540%") != std::string::npos);
        CHECK(leg_c.find("too few blocks") != std::string::npos);

        // Plain (no color, ASCII)
        Style style_p(false, false);
        std::string leg_p = legend(style_p, m);
        // Expected: '. normal  - elevated >=10%  * at risk >=20%  % overrun >=40%  . too few blocks'
        CHECK(leg_p == ". normal  - elevated >=10%  * at risk >=20%  % overrun >=40%  . too few blocks");
    }

    // ----- heatmap wrapping (BARS_PER_LINE) -----
    {
        const Metric &m = metric_by_key("lateness");
        Style style_p(false, false);
        std::vector<std::pair<std::string, std::map<int, Cell>>> rows = {
            {"Pass 1", {}}
        };
        // Fill bars 1..70
        for (int b = 1; b <= 70; ++b) {
            rows[0].second[b] = Cell{0.1, 10};
        }
        auto lines = heatmap(style_p, m, rows, 1, 70, 10);
        // Should have 2 wraps: 1-64, 65-70 -> 2 rulers + 2 rows = 4 lines
        CHECK(lines.size() == 4);
        CHECK(lines[0] == "          1       9       17      25      33      41      49      57      ");
        // Ruler for 65-70: only bar 65 is shown (first bar and (65-1)%8==0)
        CHECK(lines[2] == "          65    ");
    }

    return finish_tests();
}
