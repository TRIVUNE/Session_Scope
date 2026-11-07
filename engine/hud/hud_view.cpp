// The HUD's drawing. hud_view.h describes the look; colours and sizes come from the web HUD.

#include "hud_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

#include <imgui.h>


namespace sessionscope {

namespace {

// ------------------------------------------------------------------------------------ colours

ImU32 rgba(int r, int g, int b, float a = 1.0f) {
    return IM_COL32(r, g, b, static_cast<int>(std::lround(a * 255.0f)));
}

const ImU32 GUTTER = rgba(0x10, 0x11, 0x13);
const ImU32 BLACK = rgba(0, 0, 0);
const ImU32 TICK = rgba(0x6d, 0x71, 0x7a);
const ImU32 LINE = rgba(0x1c, 0x1d, 0x21);
const ImU32 OUTLINE = rgba(0xb8, 0xc2, 0xec);
const ImU32 TEXT = rgba(0xe9, 0xe9, 0xee);
const ImU32 PLATE = rgba(42, 44, 50, 0.82f);
const ImU32 WARN = rgba(0xff, 0x5a, 0x4f);
const ImU32 OK_GREEN = rgba(0x7f, 0xd1, 0x7a);
const ImU32 AMBER = rgba(0xff, 0xb8, 0x6b);

// Inferno as 256 RGB triples: one lookup per pixel instead of blending stops.
struct Inferno {
    std::array<unsigned char, 256 * 3> table{};
    Inferno() {
        const std::array<std::pair<float, std::array<int, 3>>, 9> stops = {{
            {0.0f, {0, 0, 4}},       {0.13f, {31, 12, 72}},  {0.25f, {85, 15, 109}},
            {0.38f, {136, 34, 106}}, {0.5f, {186, 54, 85}},  {0.63f, {227, 89, 51}},
            {0.75f, {249, 140, 10}}, {0.88f, {249, 201, 50}}, {1.0f, {252, 255, 164}},
        }};
        for (int i = 0; i < 256; ++i) {
            const float t = static_cast<float>(i) / 255.0f;
            std::size_t k = 1;
            while (k < stops.size() - 1 && t > stops[k].first) ++k;   // the stop pair around t
            const auto &[t0, c0] = stops[k - 1];
            const auto &[t1, c1] = stops[k];
            const float f = (t - t0) / (t1 - t0);
            for (int j = 0; j < 3; ++j) {
                table[static_cast<std::size_t>(i * 3 + j)] =
                    static_cast<unsigned char>(std::lround(c0[j] + (c1[j] - c0[j]) * f));
            }
        }
    }
};
const Inferno INFERNO;

ImU32 heat(double t) {   // 0..1 -> colour
    const int i = static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * 255.0)) * 3;
    return IM_COL32(INFERNO.table[i], INFERNO.table[i + 1], INFERNO.table[i + 2], 255);
}

// ------------------------------------------------------------------------------------ text

// Rounds halves up, as the web HUD's Math.round did.
long long round_half_up(double v) { return static_cast<long long>(std::floor(v + 0.5)); }

// A share as a percentage, such as "+12%"; "–" when unknown.
std::string percent(std::optional<double> v, bool with_sign = false) {
    if (!v) return "\xE2\x80\x93";
    const long long rounded = round_half_up(*v * 100.0);
    return (with_sign && *v > 0 ? "+" : "") + std::to_string(rounded) + "%";
}

std::string with_thousands(long long v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
    return (v < 0 ? "-" : "") + digits;
}

std::string number(double v, int decimals) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, v);
    return text;
}

std::string short_number(double v) {   // 4 -> "4", 44.1 -> "44.1", as the web HUD printed numbers
    char text[32];
    std::snprintf(text, sizeof(text), "%g", v);
    return text;
}

float text_width(ImFont *font, float size, const std::string &text) {
    return font->CalcTextSizeA(size, 1e9f, 0, text.c_str()).x;
}

// Text whose vertical middle is at y, like canvas textBaseline "middle".
void text_middle(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 colour, const std::string &text) {
    draw->AddText(font, size, ImVec2(x, y - size * 0.5f), colour, text.c_str());
}

// Text sitting on a baseline at y, like canvas textBaseline "alphabetic".
void text_baseline(ImDrawList *draw, ImFont *font, float size, float x, float y, ImU32 colour, const std::string &text) {
    draw->AddText(font, size, ImVec2(x, y - size * 0.8f), colour, text.c_str());
}

// One line of a readout box, in coloured pieces.
struct Piece {
    std::string text;
    ImU32 colour;
};
using BoxLine = std::vector<Piece>;

// A readout box: small text on a translucent plate. Returns its size.
ImVec2 draw_box(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImVec2 padding, const std::vector<BoxLine> &lines,
                float line_height) {
    float widest = 0;
    for (const BoxLine &line : lines) {
        float w = 0;
        for (const Piece &piece : line) w += text_width(font, size, piece.text);
        widest = std::max(widest, w);
    }
    const ImVec2 box_size(widest + 2 * padding.x, static_cast<float>(lines.size()) * line_height + 2 * padding.y);
    draw->AddRectFilled(at, ImVec2(at.x + box_size.x, at.y + box_size.y), PLATE);
    float y = at.y + padding.y + (line_height - size) * 0.5f;
    for (const BoxLine &line : lines) {
        float x = at.x + padding.x;
        for (const Piece &piece : line) {
            draw->AddText(font, size, ImVec2(x, y), piece.colour, piece.text.c_str());
            x += text_width(font, size, piece.text);
        }
        y += line_height;
    }
    return box_size;
}

// ------------------------------------------------------------------------------------ fonts

// The characters beyond plain text that the HUD shows.
const char *SPECIAL_CHARACTERS = "\xE2\x80\x93\xC2\xB7\xE2\x9C\x93\xE2\x96\xB8\xE2\x96\xBE\xE2\x8C\x84\xE2\x80\xBA\xE2\x96\xB6\xE2\x89\xA5";

const ImWchar *glyph_ranges() {
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        builder.AddText(SPECIAL_CHARACTERS);
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

// The system's font folders.
std::vector<std::filesystem::path> font_folders() {
#if defined(_WIN32)
    char windows[MAX_PATH] = {};
    GetWindowsDirectoryA(windows, MAX_PATH);
    return {std::filesystem::path(windows) / "Fonts"};
#elif defined(__APPLE__)
    return {"/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts"};
#else
    return {"/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF"};
#endif
}

// A font file by name, from the first system font folder that has it; empty if none does.
std::filesystem::path find_font(const char *file) {
    std::error_code error;
    for (const auto &folder : font_folders()) {
        const auto path = folder / file;
        if (std::filesystem::exists(path, error)) return path;
    }
    return {};
}

// Font files to try, in order: Windows first, then macOS, then Linux.
const std::initializer_list<const char *> MONO_FONTS = {"CascadiaMono.ttf", "consola.ttf", "SFNSMono.ttf",
                                                        "Menlo.ttc", "DejaVuSansMono.ttf"};
const std::initializer_list<const char *> UI_FONTS = {"segoeui.ttf", "SFNS.ttf", "Helvetica.ttc", "DejaVuSans.ttf"};
const std::initializer_list<const char *> UI_SEMIBOLD_FONTS = {"seguisb.ttf", "segoeui.ttf", "SFNS.ttf",
                                                               "Helvetica.ttc", "DejaVuSans.ttf"};
const std::initializer_list<const char *> SYMBOL_FONTS = {"seguisym.ttf", "Apple Symbols.ttf", "DejaVuSans.ttf"};

// The first font file that exists, at a pixel size, with a symbol font merged in for characters
// such as the check mark. Falls back to ImGui's own font so the HUD always has text.
// `oversample` rasterises the glyphs that much larger for a high-density screen (a Retina Mac),
// so text stays sharp; it is still drawn at `size`.
ImFont *load_font(std::initializer_list<const char *> candidates, float size, float oversample) {
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFont *font = nullptr;
    for (const char *file : candidates) {
        const std::filesystem::path path = find_font(file);
        if (!path.empty()) {
            font = atlas->AddFontFromFileTTF(path.string().c_str(), size * oversample, nullptr, glyph_ranges());
            break;
        }
    }
    if (!font) {
        ImFontConfig config;
        config.SizePixels = size * oversample;
        return atlas->AddFontDefault(&config);
    }
    for (const char *file : SYMBOL_FONTS) {
        const std::filesystem::path symbols = find_font(file);
        if (symbols.empty()) continue;
        ImFontConfig merge;
        merge.MergeMode = true;
        atlas->AddFontFromFileTTF(symbols.string().c_str(), size * oversample, &merge, glyph_ranges());
        break;
    }
    return font;
}

// ------------------------------------------------------------------------------------ the bar list

struct BarSummary {
    int bar = 0;
    std::optional<double> median;
    std::optional<double> worst;
    int blocks = 0;
    int passes = 0;
};

// Every bar of the song, regrouped from the bar map the engine already sends (one row per pass plus
// the median): regrouping, not new measurement.
std::vector<BarSummary> summarise_bars(const std::vector<BarRow> &rows) {
    const bool has_median_row = std::any_of(rows.begin(), rows.end(), [](const BarRow &r) { return r.pass == 0; });
    std::map<int, BarSummary> by_bar;
    for (const BarRow &row : rows) {
        for (const auto &[bar, cell] : row.cells) {
            BarSummary &summary = by_bar[bar];
            summary.bar = bar;
            if (row.pass == 0) {
                summary.median = cell.value;
                continue;
            }
            summary.blocks += cell.n;
            if (cell.value) {
                ++summary.passes;
                summary.worst = summary.worst ? std::max(*summary.worst, *cell.value) : *cell.value;
            }
            if (!has_median_row) summary.median = cell.value;   // a single pass: its value is the median
        }
    }
    std::vector<BarSummary> out;
    for (const auto &[bar, summary] : by_bar) out.push_back(summary);
    return out;
}

std::string status_of(const BarSummary &bar, const Metric &metric) {
    if (!bar.median) return "";
    if (*bar.median >= metric.tiers[2]) return "overrun";
    if (*bar.median >= metric.tiers[1]) return "at risk";
    if (*bar.median >= metric.tiers[0]) return "elevated";
    return "";
}

// Amber tint by load, stronger with the value; red once it overruns, like Task Manager's heat cells.
std::optional<ImU32> tint(std::optional<double> v, const Metric &metric) {
    if (!v) return std::nullopt;
    const float strength = static_cast<float>(std::min(1.0, *v / metric.scale));
    if (*v >= metric.tiers[2]) return rgba(255, 90, 79, 0.18f + 0.3f * strength);
    return rgba(255, 190, 40, 0.05f + 0.45f * strength);
}

std::function<bool(const BarSummary &, const BarSummary &)> sort_order(const std::string &key) {
    const auto descending = [](std::optional<double> a, std::optional<double> b) { return a.value_or(-1) > b.value_or(-1); };
    if (key == "status" || key == "med") return [=](const BarSummary &a, const BarSummary &b) { return descending(a.median, b.median); };
    if (key == "worst") return [=](const BarSummary &a, const BarSummary &b) { return descending(a.worst, b.worst); };
    if (key == "blocks") return [](const BarSummary &a, const BarSummary &b) { return a.blocks > b.blocks; };
    if (key == "passes") return [](const BarSummary &a, const BarSummary &b) { return a.passes > b.passes; };
    return [](const BarSummary &a, const BarSummary &b) { return a.bar < b.bar; };
}

// The spectrogram's vertical axis: lateness -> 0..1. The engine uses the same formula.
double spectrogram_position(double v) { return spec_pos(v); }

const std::array<double, 6> LATENESS_TICKS = {0, 0.1, 0.2, 0.4, 1, 2};   // labelled levels, as a share of a block
constexpr int SPAN = 240;                                                  // columns shown: 12 s at 50 ms

}  // namespace

// ------------------------------------------------------------------------------------ setup

HudView::HudView(TextureStore &textures, double layout_scale, double font_oversample)
    : scale_(static_cast<float>(layout_scale)), textures_(textures) {
    const float oversample = static_cast<float>(font_oversample);
    mono9_ = load_font(MONO_FONTS, px(9), oversample);
    mono10_ = load_font(MONO_FONTS, px(10), oversample);
    mono11_ = load_font(MONO_FONTS, px(11), oversample);
    mono13_ = load_font(MONO_FONTS, px(13), oversample);
    ui13_ = load_font(UI_FONTS, px(13), oversample);
    ui15_ = load_font(UI_SEMIBOLD_FONTS, px(15), oversample);
    ui17_ = load_font(UI_FONTS, px(17), oversample);
    ImGui::GetIO().FontDefault = mono11_;
}

HudView::~HudView() {
    if (texture_) textures_.destroy_texture(texture_);
}

void HudView::receive(const Snapshot &snapshot) {
    for (const SpectrogramColumn &column : snapshot.spec_cols) {
        columns_[column.id] = column.bins;
        newest_column_ = newest_column_ ? std::max(*newest_column_, column.id) : column.id;
    }
    if (!snapshot.spec_cols.empty()) {
        // Keep only what is visible, and restart the slide.
        while (!columns_.empty() && columns_.begin()->first < *newest_column_ - SPAN) columns_.erase(columns_.begin());
        newest_arrived_at_ = ImGui::GetTime();
        texture_dirty_ = true;
    }
    state_ = snapshot;
    have_state_ = true;
}

// ------------------------------------------------------------------------------------ layout

HudView::Layout HudView::layout(float width, float height) const {
    const float gap = px(2);
    if (compact_) {   // the waveform and the readout fill the window; nothing else is drawn
        // The readout keeps its full 300 px if it can: in a short strip it needs the width.
        const float readout_width = std::clamp(width * 0.48f, px(260), px(300));
        Layout l{};
        l.wave = {0, 0, width - readout_width - gap, height};
        l.read = {width - readout_width, 0, readout_width, height};
        return l;
    }
    const float right_width = std::clamp(width * 0.26f, px(180), px(300));
    const float left_width = width - right_width - gap;

    // The meters take 58% of the height when the list is open, all of it otherwise.
    const float meters_height = list_open_ ? std::max(px(300), height * 0.58f) : height;

    const std::size_t pass_rows = have_state_ && state_.ready ? state_.bars.size() : 0;
    const float bars_height = px(14 + std::max<std::size_t>(1, pass_rows) * 9 + 4);   // 9 px per row under 14 px of ticks
    const float wave_height = std::clamp(height * 0.17f, px(70), px(170));
    const float spec_height = std::max(px(40), meters_height - bars_height - wave_height - 2 * gap);

    const float right_x = left_width + gap;
    const float bars_y = spec_height + gap;
    const float wave_y = bars_y + bars_height + gap;
    Layout l;
    l.spec = {0, 0, left_width, spec_height};
    l.prof = {right_x, 0, right_width, spec_height};
    l.bars = {0, bars_y, left_width, bars_height};
    l.bars_right = {right_x, bars_y, right_width, bars_height};
    l.wave = {0, wave_y, left_width, wave_height};
    l.read = {right_x, wave_y, right_width, wave_height};
    const float list_y = wave_y + wave_height + gap;
    l.list = {0, list_y, width, std::max(0.0f, height - list_y)};
    return l;
}

ViewRequests HudView::draw(bool sensor_installed) {
    ViewRequests requests;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const Layout l = layout(size.x, size.y);

    draw->AddRectFilled(ImVec2(0, 0), size, GUTTER);   // the gutters between the graphics
    if (!compact_) {
        draw_spectrogram(draw, l.spec);
        draw_waiting_checklist(draw, l.spec, sensor_installed);
        draw_profile(draw, l.prof);
        draw_transport(draw, l.prof);
        draw_bar_map(draw, l.bars, l.bars_right);
    }
    draw_waveform(draw, l.wave);
    draw_readout(draw, l.read, requests);
    if (!compact_ && list_open_ && l.list.h > px(20)) draw_task_manager(l.list);
    return requests;
}

void HudView::apply(const HudSettings &settings) {
    on_top_ = settings.on_top;
    list_open_ = settings.list_open;
    compact_ = settings.compact_mode;
    // Only a column the list has: the file may have been edited by hand.
    for (const char *key : {"bar", "status", "med", "worst", "blocks", "passes"}) {
        if (settings.sort_key == key) sort_key_ = key;
    }
}

void HudView::remember(HudSettings &settings) const {
    settings.on_top = on_top_;
    settings.list_open = list_open_;
    settings.compact_mode = compact_;
    settings.sort_key = sort_key_;
}

// ------------------------------------------------------------------------------------ spectrogram

void HudView::rebuild_spectrogram_texture() {
    // One pixel per column and bin: oldest column on the left, bin 0 (early) at the bottom.
    std::vector<unsigned char> pixels(static_cast<std::size_t>(SPAN * SPEC_BINS * 4), 0);
    for (int c = 0; c < SPAN; ++c) {
        const auto found = columns_.find(*newest_column_ - (SPAN - 1 - c));
        if (found == columns_.end() || found->second.empty()) continue;   // nothing playing: stays black
        const std::vector<int> &bins = found->second;
        int total = 0;
        for (const int n : bins) total += n;
        for (int b = 0; b < SPEC_BINS && b < static_cast<int>(bins.size()); ++b) {
            if (!bins[static_cast<std::size_t>(b)]) continue;
            // The bin's share of the column's blocks, lifted so faint bins still show.
            const double t = std::pow(static_cast<double>(bins[static_cast<std::size_t>(b)]) / total, 0.45);
            const int level = static_cast<int>(std::lround(std::min(1.0, 0.18 + 0.82 * t) * 255.0)) * 3;
            const std::size_t p = static_cast<std::size_t>(((SPEC_BINS - 1 - b) * SPAN + c) * 4);
            pixels[p] = INFERNO.table[level];
            pixels[p + 1] = INFERNO.table[level + 1];
            pixels[p + 2] = INFERNO.table[level + 2];
            pixels[p + 3] = 255;
        }
    }
    texture_ = textures_.upload_texture(texture_, SPAN, SPEC_BINS, reinterpret_cast<const std::uint32_t *>(pixels.data()));
    texture_dirty_ = false;
}

void HudView::draw_spectrogram(ImDrawList *draw, const Rect &r) {
    draw->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), BLACK);
    if (!have_state_ || !newest_column_) return;
    if (texture_dirty_) rebuild_spectrogram_texture();

    // Slide left with the time since the newest column arrived (at most 2 columns, one engine
    // frame), so the picture moves every display frame instead of jumping ten times a second.
    const float column_width = r.w / SPAN;
    const double since = (ImGui::GetTime() - newest_arrived_at_) / state_.spec_col_s;
    const float slide = state_.playing ? static_cast<float>(std::min(2.0, since)) * column_width : 0.0f;

    draw->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), true);
    draw->AddImage(static_cast<ImTextureID>(texture_), ImVec2(r.x - slide, r.y), ImVec2(r.x - slide + r.w, r.y + r.h));
    draw_spectrogram_axes(draw, r);
    draw->PopClipRect();
    draw_spectrogram_hover(draw, r);
}

void HudView::draw_spectrogram_axes(ImDrawList *draw, const Rect &r) {
    // Lateness levels as faint lines, labelled at the left; +40% (an overrun) in red.
    for (const double v : LATENESS_TICKS) {
        const float y = std::round(r.y + r.h - static_cast<float>(spectrogram_position(v)) * r.h);
        const ImU32 colour = v == 0.4 ? rgba(255, 90, 79, 0.35f) : rgba(255, 255, 255, 0.07f);
        draw->AddRectFilled(ImVec2(r.x + px(28), y), ImVec2(r.x + r.w, y + px(1)), colour);
        text_middle(draw, mono10_, px(10), r.x + px(3), y, TICK, v == 0 ? "0" : percent(v, true));
    }
    // Time ticks along the bottom.
    const double span_seconds = SPAN * state_.spec_col_s;
    for (int k = 1; k < 4; ++k) {
        const std::string label = "-" + short_number(span_seconds * (4 - k) / 4) + "s";
        const float x = r.x + r.w * k / 4 - text_width(mono10_, px(10), label) * 0.5f;
        text_baseline(draw, mono10_, px(10), x, r.y + r.h - px(4), TICK, label);
    }
}

void HudView::draw_spectrogram_hover(ImDrawList *draw, const Rect &r) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < r.x || mouse.x >= r.x + r.w || mouse.y < r.y || mouse.y >= r.y + r.h || !newest_column_) return;
    const double across = (mouse.x - r.x) / r.w;
    const double up = 1.0 - (mouse.y - r.y) / r.h;
    const double age = (1.0 - across) * SPAN * state_.spec_col_s;   // seconds before now

    // Invert the axis to find the lateness under the mouse.
    const double lo = std::asinh(state_.spec_k * state_.spec_lo);
    const double hi = std::asinh(state_.spec_k * state_.spec_hi);
    const double lateness = std::sinh(lo + up * (hi - lo)) / state_.spec_k;

    int blocks = 0;
    const auto found = columns_.find(*newest_column_ - static_cast<int>(std::lround((1.0 - across) * (SPAN - 1))));
    if (found != columns_.end() && !found->second.empty()) {
        const int bin = std::min(state_.spec_bins - 1, static_cast<int>(std::floor(up * state_.spec_bins)));
        blocks = found->second[static_cast<std::size_t>(bin)];
    }
    draw_box(draw, mono11_, px(11), ImVec2(r.x + px(8), r.y + px(8)), ImVec2(px(8), px(4)),
             {{{"-" + number(age, 2) + "s", TEXT}}, {{percent(lateness, true) + " late", TEXT}},
              {{std::to_string(blocks) + " blocks", TEXT}}},
             px(11 * 1.45f));
}

// Waiting: a plain checklist in the middle of the spectrogram.
void HudView::draw_waiting_checklist(ImDrawList *draw, const Rect &r, bool sensor_installed) {
    if (!have_state_) return;
    const bool waiting = !state_.ready || (!state_.playing && state_.status != "stale" && columns_.empty());
    if (!waiting) return;

    struct Step {
        bool done;
        std::string text;
    };
    const std::vector<Step> steps = {
        {sensor_installed, sensor_installed ? "sensor plug-in installed" : "run  python tools/setup.py"},
        {state_.status != "waiting", "put SessionScope Sensor on the master track"},
        {state_.playing, "press play"},
    };
    const float line_height = px(11 * 1.8f);
    // No heading: the box at the top right already says NO SIGNAL, or STOP once the sensor is seen.
    float widest = 0;
    for (const Step &step : steps) widest = std::max(widest, text_width(mono11_, px(11), "! " + step.text));
    const float block_height = line_height * static_cast<float>(steps.size());
    const float x = r.x + (r.w - widest) * 0.5f;
    float y = r.y + (r.h - block_height) * 0.5f;

    for (const Step &step : steps) {
        const std::string mark = step.done ? "\xE2\x9C\x93" : "!";
        draw->AddText(mono11_, px(11), ImVec2(x, y), step.done ? OK_GREEN : AMBER, mark.c_str());
        draw->AddText(mono11_, px(11), ImVec2(x + text_width(mono11_, px(11), mark + " "), y), TICK, step.text.c_str());
        y += line_height;
    }
}

// ------------------------------------------------------------------------------------ profile

// The spectrogram's bins summed over the visible window, as coloured bands plus an outline, on the
// same vertical axis.
void HudView::draw_profile(ImDrawList *draw, const Rect &r) {
    draw->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), BLACK);
    if (!have_state_ || !newest_column_) return;

    std::vector<double> sums(static_cast<std::size_t>(state_.spec_bins), 0.0);
    for (int c = *newest_column_ - SPAN + 1; c <= *newest_column_; ++c) {
        const auto found = columns_.find(c);
        if (found == columns_.end()) continue;
        for (std::size_t b = 0; b < found->second.size() && b < sums.size(); ++b) sums[b] += found->second[b];
    }
    const double largest = std::max(1.0, *std::max_element(sums.begin(), sums.end()));
    const float band = r.h / state_.spec_bins;
    const float right = r.w - px(34);   // labels on the right

    std::vector<ImVec2> outline;
    for (std::size_t b = 0; b < sums.size(); ++b) {
        const float f = static_cast<float>(std::sqrt(sums[b] / largest));   // square root: small counts stay visible
        const float y = r.y + r.h - static_cast<float>(b + 1) * band;
        if (sums[b] > 0) draw->AddRectFilled(ImVec2(r.x, y), ImVec2(r.x + f * right, y + band + 0.5f), heat(0.2 + 0.8 * f));
        outline.emplace_back(r.x + f * right, y + band / 2);   // the outline through the band tips
    }
    draw->AddPolyline(outline.data(), static_cast<int>(outline.size()), OUTLINE, ImDrawFlags_None, px(1));

    for (const double v : LATENESS_TICKS) {
        const float y = std::round(r.y + r.h - static_cast<float>(spectrogram_position(v)) * r.h);
        draw->AddRectFilled(ImVec2(r.x, y), ImVec2(r.x + right, y + px(1)), LINE);
        const std::string label = v == 0 ? "0" : percent(v, true);
        text_middle(draw, mono10_, px(10), r.x + r.w - px(4) - text_width(mono10_, px(10), label), y, TICK, label);
    }
}

// Position, tempo, format, and any warning, over the top of the profile.
void HudView::draw_transport(ImDrawList *draw, const Rect &r) {
    std::vector<BoxLine> lines;
    if (!have_state_) {
        lines.push_back({{"STARTING", TICK}});
    } else {
        const Snapshot &s = state_;
        if (s.second_sensor) lines.push_back({{"TWO SENSORS ON THE PROJECT", WARN}});
        else if (s.status == "stale") lines.push_back({{"SENSOR GONE", WARN}});
        if (s.ready) {
            std::string bar = s.bar ? std::to_string(*s.bar) : "\xE2\x80\x93";
            bar.resize(std::max<std::size_t>(bar.size(), 5), ' ');
            lines.push_back({{"BAR " + bar + (s.playing ? "PLAY" : "STOP") + (s.pass ? "  P" + std::to_string(*s.pass) : ""), TEXT}});
            const std::string signature = (s.ts[0] ? short_number(*s.ts[0]) : "?") + "/" + (s.ts[1] ? short_number(*s.ts[1]) : "?");
            lines.push_back({{number(s.tempo.value_or(0), 1) + " BPM  " + signature, TEXT}});
            lines.push_back({{short_number(s.srate.value_or(0) / 1000) + "k  " + short_number(s.frames.value_or(0)) + "  " +
                              number(s.deadline_ms.value_or(0), 2) + "ms", TEXT}});
        } else {
            lines.push_back({{"NO SIGNAL", TEXT}});
        }
        if (s.note) lines.push_back({{s.source == "demo" ? "DEMO DATA" : "DUMMY DEVICE \xC2\xB7 REPLAY", TICK}});
    }
    draw_box(draw, mono11_, px(11), ImVec2(r.x + px(8), r.y + px(8)), ImVec2(px(8), px(4)), lines, px(11 * 1.45f));
}

// ------------------------------------------------------------------------------------ bar map

// One thin row per pass (newest at the bottom, then the median), one cell per bar.
void HudView::draw_bar_map(ImDrawList *draw, const Rect &r, const Rect &right) {
    if (!have_state_ || !state_.ready || state_.bars.empty() || !state_.metric) return;
    const Snapshot &s = state_;
    int first = 1 << 30, last = -(1 << 30);
    for (const BarRow &row : s.bars) {
        for (const auto &[bar, cell] : row.cells) {
            first = std::min(first, bar);
            last = std::max(last, bar);
        }
    }
    if (first > last) return;
    const int span = std::max(last - first + 1, 16);   // at least 16 bars wide
    const float cell_width = r.w / span, row_height = px(9), top = px(14);
    int step = 128;
    for (const int k : {1, 2, 4, 8, 16, 32, 64}) {
        if (k * cell_width >= px(26)) {
            step = k;
            break;
        }
    }
    for (int bar = first; bar < first + span; ++bar) {   // bar numbers along the top
        if ((bar - 1) % step == 0) text_baseline(draw, mono10_, px(10), r.x + (bar - first) * cell_width + px(2), r.y + px(10), TICK, std::to_string(bar));
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    std::optional<std::string> hover;
    for (std::size_t i = 0; i < s.bars.size(); ++i) {
        const BarRow &row = s.bars[i];
        const float y = r.y + top + static_cast<float>(i) * row_height;
        for (const auto &[bar, cell] : row.cells) {
            const float x = r.x + (bar - first) * cell_width;
            const ImU32 colour = cell.value ? heat(0.15 + 0.85 * std::min(1.0, *cell.value / s.metric->scale)) : rgba(0x1a, 0x1a, 0x1f);
            const float w = std::max(px(1), cell_width - (cell_width > px(5) ? px(1) : 0));
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + row_height - px(1)), colour);
            if (mouse.x >= x && mouse.x < x + cell_width && mouse.y >= y && mouse.y < y + row_height) {
                std::string label = row.label;
                std::transform(label.begin(), label.end(), label.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                hover = "bar " + std::to_string(bar) + "  " + label + "  " + (cell.value ? percent(*cell.value, true) + " late" : "too few blocks");
            }
        }
    }
    if (s.playing && s.bar && *s.bar >= first && *s.bar < first + span) {   // the bar playing now: a white marker
        const float x = r.x + (*s.bar - first) * cell_width;
        draw->AddRectFilled(ImVec2(x, r.y + top - px(3)), ImVec2(x + std::max(px(2), cell_width - px(1)), r.y + top - px(1)), IM_COL32_WHITE);
    }

    // Row names in the strip on the right.
    text_middle(draw, mono9_, px(9), right.x + px(6), right.y + px(7), TICK, "bars \xC2\xB7 late p99");
    for (std::size_t i = 0; i < s.bars.size(); ++i) {
        const BarRow &row = s.bars[i];
        const ImU32 colour = row.pass == 0 ? TEXT : (s.pass && row.pass == *s.pass) ? AMBER : TICK;
        text_middle(draw, mono9_, px(9), right.x + px(6), right.y + top + static_cast<float>(i) * row_height + px(4), colour,
                    row.pass == 0 ? "median" : "pass " + std::to_string(row.pass));
    }
    if (hover) {
        const float x = std::min(r.x + (mouse.x - r.x) + px(8), r.x + r.w - px(220));
        draw_box(draw, mono11_, px(11), ImVec2(x, r.y), ImVec2(px(8), px(4)), {{{*hover, TEXT}}}, px(11 * 1.45f));
    }
}

// ------------------------------------------------------------------------------------ waveform

// The worst lateness per 50 ms slice, mirrored around the middle, coloured by level.
void HudView::draw_waveform(ImDrawList *draw, const Rect &r) {
    draw->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), BLACK);
    const float mid = r.y + r.h / 2, half = r.h / 2 - px(4);
    draw->AddRectFilled(ImVec2(r.x, std::round(mid)), ImVec2(r.x + r.w, std::round(mid) + px(1)), LINE);   // the zero line
    const double scale = have_state_ ? state_.late_scale : 0.45;
    for (const double v : {0.1, 0.2, 0.4}) {   // level ticks on both halves, like "-6dB"
        const float d = static_cast<float>(v / scale) * half;
        const ImU32 colour = v == 0.4 ? rgba(255, 90, 79, 0.3f) : LINE;
        for (const float y : {std::round(mid - d), std::round(mid + d)}) {
            draw->AddRectFilled(ImVec2(r.x, y), ImVec2(r.x + r.w - px(40), y + px(1)), colour);
            const std::string label = percent(v, true);
            text_middle(draw, mono10_, px(10), r.x + r.w - px(4) - text_width(mono10_, px(10), label), y, TICK, label);
        }
    }
    if (!have_state_ || !state_.ready || state_.meter.empty()) return;
    const float column_width = (r.w - px(40)) / static_cast<float>(state_.meter.size());
    for (std::size_t c = 0; c < state_.meter.size(); ++c) {
        const std::optional<double> v = state_.meter[c];
        if (!v || *v <= 0) continue;   // early or on time: nothing to draw
        const double f = std::min(1.0, *v / scale);
        const float d = std::max(px(1), static_cast<float>(f) * half);
        const float x = r.x + static_cast<float>(c) * column_width;
        // Never darker than purple, so it shows on black.
        draw->AddRectFilled(ImVec2(x, mid - d), ImVec2(x + std::max(px(1), column_width - px(0.3f)), mid + d), heat(0.3 + 0.7 * f));
    }
}

// ------------------------------------------------------------------------------------ readout

// The big number, the level meter, the counters, and the two small buttons.
void HudView::draw_readout(ImDrawList *draw, const Rect &r, ViewRequests &requests) {
    draw->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), BLACK);
    const bool ready = have_state_ && state_.ready;
    const std::optional<double> late = ready ? state_.late.p99 : std::nullopt;
    const bool overrun = ready && state_.late.tier && *state_.late.tier >= 3;
    draw_box(draw, mono13_, px(13), ImVec2(r.x + px(10), r.y + px(10)), ImVec2(px(9), px(5)),
             {{{"LATE p99 ", TEXT}, {percent(late, true), overrun ? WARN : TEXT}}}, px(13 * 1.45f));

    if (ready) {
        const PassWorst &worst = state_.pass_worst;
        const std::string hottest = worst.value && worst.bar ? "bar " + std::to_string(*worst.bar) + "  " + percent(*worst.value, true) : "\xE2\x80\x93";
        const std::vector<std::pair<std::string, std::string>> facts = {
            {"hottest ", hottest}, {"lost    ", std::to_string(state_.lost)}, {"source  ", state_.source}};
        const float line_height = px(11 * 1.45f);
        float y = r.y + r.h - px(10) - line_height * static_cast<float>(facts.size());
        for (const auto &[name, value] : facts) {
            draw->AddText(mono11_, px(11), ImVec2(r.x + px(10), y), TICK, name.c_str());
            draw->AddText(mono11_, px(11), ImVec2(r.x + px(10) + text_width(mono11_, px(11), name), y), TEXT, value.c_str());
            y += line_height;
        }
    }

    const Rect level = {r.x + r.w - px(10) - px(46), r.y + px(10), px(46), r.h - px(20)};
    draw_level_meter(draw, level);

    // The pin (keep on top), the list toggle and compact mode, as plain words.
    const auto word_button = [&](const char *id, const std::string &label, float top, bool lit) {
        const float w = text_width(mono11_, px(11), label) + px(8);
        const ImVec2 at(r.x + r.w - px(62) - w, r.y + top);
        ImGui::SetCursorScreenPos(at);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, px(16)));
        draw->AddText(mono11_, px(11), ImVec2(at.x + px(4), at.y + px(2)), lit || ImGui::IsItemHovered() ? TEXT : TICK, label.c_str());
        return clicked;
    };
    if (word_button("##pin", "pin", px(10), on_top_)) {
        on_top_ = !on_top_;
        requests.keep_on_top = on_top_;
    }
    if (!compact_ && word_button("##list", list_open_ ? "list \xE2\x96\xBE" : "list \xE2\x96\xB8", px(28), false)) {
        list_open_ = !list_open_;
    }
    // In compact mode the list button is gone, so "full" moves up into its place.
    if (word_button("##compact", compact_ ? "full" : "compact", compact_ ? px(28) : px(46), false)) {
        compact_ = !compact_;
        requests.compact = compact_;
    }
}

// The 1 s p99 as a vertical bar with a 3-second peak hold, ticks on the left.
void HudView::draw_level_meter(ImDrawList *draw, const Rect &r) {
    const double scale = have_state_ ? state_.late_scale : 0.45;
    const float bar_x = r.x + r.w - px(10), bar_w = px(8);
    draw->AddRectFilled(ImVec2(bar_x, r.y), ImVec2(bar_x + bar_w, r.y + r.h), rgba(0x12, 0x13, 0x16));
    for (const double v : {0.0, 0.1, 0.2, 0.4}) {
        const float y = std::clamp(r.y + r.h - static_cast<float>(v / scale) * r.h, r.y + px(5), r.y + r.h - px(5));
        const std::string label = v > 0 ? std::to_string(round_half_up(v * 100)) : "0";
        text_middle(draw, mono10_, px(10), bar_x - px(4) - text_width(mono10_, px(10), label), y, TICK, label);
    }
    if (!have_state_ || !state_.ready || !state_.late.p99) return;
    const double v = std::max(0.0, *state_.late.p99), f = std::min(1.0, v / scale);
    const double now = ImGui::GetTime();
    if (v >= peak_value_ || now - peak_time_ > 3.0) {   // hold the peak for 3 s
        peak_value_ = v;
        peak_time_ = now;
    }
    draw->AddRectFilled(ImVec2(bar_x, r.y + r.h - static_cast<float>(f) * r.h), ImVec2(bar_x + bar_w, r.y + r.h), heat(0.3 + 0.7 * f));
    const float peak_y = std::round(r.y + r.h - static_cast<float>(std::min(1.0, peak_value_ / scale)) * r.h);
    draw->AddRectFilled(ImVec2(bar_x, peak_y), ImVec2(bar_x + bar_w, peak_y + px(2)), IM_COL32_WHITE);
}

// ------------------------------------------------------------------------------------ Task Manager

// The opened bar's blocks, one line per pass, under its row: how many, the median, the p99, the
// worst single block and how many overran. Text only, in the list's dim colour.
void HudView::draw_bar_detail(ImDrawList *rows, const Rect &r, float indent, const Metric &metric) {
    const float line_height = px(22);
    const std::vector<BarPassDetail> &detail = state_.selected_bar == selected_bar_ ? state_.selected_detail
                                                                                     : std::vector<BarPassDetail>{};
    const std::string overrun_line = percent(metric.tiers[2]);
    std::vector<std::string> lines;
    for (const BarPassDetail &d : detail) {
        lines.push_back("pass " + std::to_string(d.pass) + "   " + with_thousands(d.blocks) + " blocks   median " +
                        percent(d.median) + "   p99 " + percent(d.p99) + "   worst " + percent(d.worst) + "   " +
                        std::to_string(d.overruns) + " at or over " + overrun_line);
    }
    if (lines.empty()) lines.push_back("no blocks yet");   // the first tick after the click
    for (const std::string &line : lines) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(r.w, line_height));
        rows->AddText(mono11_, px(11), ImVec2(at.x + indent * 0.16f, at.y + (line_height - px(11)) * 0.5f), TICK, line.c_str());
    }
    ImGui::Dummy(ImVec2(r.w, px(6)));
}

// Every bar of the song as a row, grouped "Playing now / Hot bars / All bars", totals over the
// column names, amber-tinted cells. Click a column to sort by it, a group title to fold it.
void HudView::draw_task_manager(const Rect &r) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), rgba(0x19, 0x19, 0x19));
    draw->AddText(ui15_, px(15), ImVec2(r.x + px(16), r.y + px(10)), IM_COL32_WHITE, "Bars");
    if (!have_state_ || !state_.ready || !state_.metric) return;

    const Snapshot &s = state_;
    const Metric &metric = *s.metric;
    std::vector<BarSummary> bars = summarise_bars(s.bars);

    // Columns: Name 34%, Status 16%, the four numbers sharing the rest.
    struct Column {
        const char *key;
        const char *name;
        float width;
        bool left;
    };
    const float w = r.w;
    const std::array<Column, 6> columns = {{{"bar", "Name", w * 0.34f, true}, {"status", "Status", w * 0.16f, true},
                                            {"med", "Late p99", w * 0.125f, false}, {"worst", "Worst pass", w * 0.125f, false},
                                            {"blocks", "Blocks", w * 0.125f, false}, {"passes", "Passes", w * 0.125f, false}}};

    double worst_overall = 0;
    long long total_blocks = 0;
    for (const BarSummary &bar : bars) {
        worst_overall = std::max(worst_overall, bar.worst.value_or(0));
        total_blocks += bar.blocks;
    }
    const long long pass_rows = std::count_if(s.bars.begin(), s.bars.end(), [](const BarRow &row) { return row.pass > 0; });
    const std::map<std::string, std::string> totals = {
        {"bar", ""}, {"status", ""}, {"med", percent(s.late.p99)}, {"worst", percent(worst_overall)},
        {"blocks", with_thousands(total_blocks)}, {"passes", std::to_string(pass_rows)}};

    // The header: the total above the column name, like Task Manager's "10% CPU".
    const float header_top = r.y + px(34), header_height = px(48);
    float x = r.x;
    for (const Column &column : columns) {
        const bool sorted = sort_key_ == column.key;
        if (sorted) draw->AddRectFilled(ImVec2(x, header_top), ImVec2(x + column.width, header_top + header_height), rgba(0x26, 0x26, 0x26));
        if (x > r.x) draw->AddRectFilled(ImVec2(x, header_top), ImVec2(x + px(1), header_top + header_height), rgba(0x2b, 0x2b, 0x2b));
        const std::string name = std::string(column.name) + (sorted ? " \xE2\x96\xBE" : "");
        const std::string &total = totals.at(column.key);
        const auto place = [&](ImFont *font, float size, const std::string &text) {
            return column.left ? x + px(column.key == std::string("bar") ? 16 : 12) : x + column.width - px(12) - text_width(font, size, text);
        };
        draw->AddText(ui17_, px(17), ImVec2(place(ui17_, px(17), total), header_top + px(4)), IM_COL32_WHITE, total.c_str());
        draw->AddText(ui13_, px(13), ImVec2(place(ui13_, px(13), name), header_top + px(26)), rgba(0xc9, 0xc9, 0xc9), name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x, header_top));
        if (ImGui::InvisibleButton((std::string("##sort_") + column.key).c_str(), ImVec2(column.width, header_height))) sort_key_ = column.key;
        x += column.width;
    }
    draw->AddRectFilled(ImVec2(r.x, header_top + header_height), ImVec2(r.x + r.w, header_top + header_height + px(1)), rgba(0x2b, 0x2b, 0x2b));

    // The rows scroll under the fixed header.
    const auto order = sort_order(sort_key_);
    std::vector<BarSummary> playing_now, hot;
    for (const BarSummary &bar : bars) {
        if (s.playing && s.bar && bar.bar == *s.bar) playing_now.push_back(bar);
        if (bar.median && *bar.median >= metric.tiers[1]) hot.push_back(bar);
    }
    // A bar that just began has no blocks in the chart yet (it updates twice a second). Show it
    // anyway, with dashes, so the list doesn't jump at the start of every bar.
    if (s.playing && s.bar && *s.bar > 0 && playing_now.empty()) {
        BarSummary starting;
        starting.bar = *s.bar;
        playing_now.push_back(starting);
    }
    std::sort(hot.begin(), hot.end(), order);
    std::sort(bars.begin(), bars.end(), order);

    const float rows_top = header_top + header_height + px(1);
    ImGui::SetCursorScreenPos(ImVec2(r.x, rows_top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##bar_list", ImVec2(r.w, r.y + r.h - rows_top), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList *rows = ImGui::GetWindowDrawList();

    const auto group = [&](const char *id, const char *title, const std::vector<BarSummary> &list) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton((std::string("##group_") + id).c_str(), ImVec2(r.w, px(36)))) group_open_[id] = !group_open_[id];
        const std::string chevron = group_open_[id] ? "\xE2\x8C\x84" : "\xE2\x80\xBA";
        rows->AddText(ui15_, px(15), ImVec2(at.x + px(16), at.y + px(12)), rgba(0xc9, 0xc9, 0xc9), chevron.c_str());
        const std::string label = std::string(title) + " (" + std::to_string(list.size()) + ")";
        rows->AddText(ui15_, px(15), ImVec2(at.x + px(34), at.y + px(12)), IM_COL32_WHITE, label.c_str());
        if (!group_open_[id]) return;

        for (const BarSummary &bar : list) {
            const ImVec2 row_at = ImGui::GetCursorScreenPos();
            const bool opened = selected_bar_ && *selected_bar_ == bar.bar;
            if (ImGui::InvisibleButton(("##row_" + std::string(id) + std::to_string(bar.bar)).c_str(), ImVec2(r.w, px(28)))) {
                selected_bar_ = opened ? std::nullopt : std::optional<int>(bar.bar);   // click again to close
            }
            if (opened || ImGui::IsItemHovered()) rows->AddRectFilled(row_at, ImVec2(row_at.x + r.w, row_at.y + px(28)), rgba(0x2d, 0x2d, 0x2d));
            const float text_y = row_at.y + (px(28) - px(13)) * 0.5f;
            const bool now = s.playing && s.bar && bar.bar == *s.bar;
            const std::string status = status_of(bar, metric);

            float cx = row_at.x;
            // Name: a colour square, then "Bar N".
            const ImU32 square = bar.median ? heat(0.15 + 0.85 * std::min(1.0, *bar.median / metric.scale)) : rgba(0x33, 0x33, 0x33);
            const float square_y = row_at.y + (px(28) - px(10)) * 0.5f;
            rows->AddRectFilled(ImVec2(cx + px(34), square_y), ImVec2(cx + px(44), square_y + px(10)), square, px(2));
            rows->AddText(ui13_, px(13), ImVec2(cx + px(54), text_y), IM_COL32_WHITE, ("Bar " + std::to_string(bar.bar)).c_str());
            cx += columns[0].width;

            // Status: "playing" with a green mark, or the tier.
            ImU32 status_colour = rgba(0xc9, 0xc9, 0xc9);
            if (status == "overrun") status_colour = rgba(0xff, 0x8a, 0x80);
            if (status == "at risk") status_colour = AMBER;
            float sx = cx + px(12);
            if (now) {
                rows->AddText(ui13_, px(13), ImVec2(sx, text_y), OK_GREEN, "\xE2\x96\xB6 ");
                sx += text_width(ui13_, px(13), "\xE2\x96\xB6 ");
            }
            rows->AddText(ui13_, px(13), ImVec2(sx, text_y), now ? rgba(0xc9, 0xc9, 0xc9) : status_colour, (now ? "playing" : status).c_str());
            cx += columns[1].width;

            // The numbers, right-aligned, with heat tints on the two values.
            const std::array<std::pair<std::string, std::optional<double>>, 4> cells = {{
                {percent(bar.median), bar.median}, {percent(bar.worst), bar.worst},
                {with_thousands(bar.blocks), std::nullopt}, {std::to_string(bar.passes), std::nullopt}}};
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const float cw = columns[i + 2].width;
                if (const auto colour = tint(cells[i].second, metric)) {   // a 1 px gap keeps neighbouring cells apart
                    rows->AddRectFilled(ImVec2(cx + px(1), row_at.y), ImVec2(cx + cw, row_at.y + px(28)), *colour);
                }
                const std::string &text = cells[i].first;
                rows->AddText(ui13_, px(13), ImVec2(cx + cw - px(12) - text_width(ui13_, px(13), text), text_y), IM_COL32_WHITE, text.c_str());
                cx += cw;
            }
            if (opened) draw_bar_detail(rows, r, columns[0].width, metric);
        }
    };
    if (!playing_now.empty()) group("now", "Playing now", playing_now);
    group("hot", "Hot bars", hot);
    group("all", "All bars", bars);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

}  // namespace sessionscope
