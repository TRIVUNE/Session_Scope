// =================================================================================================
// hud_view.cpp - lays the modules out in a strip, and the right-click menu (see hud_view.h)
// =================================================================================================

#include "hud_view.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "look/fonts_data.h"
#include "modules/module_list.h"

namespace sessionscope {

namespace {

// Space around the strip and between modules, in design pixels.
constexpr float EDGE = 8;
constexpr float GAP = 8;

// Turns a font compiled into the program (look/fonts_data.h) into an ImGui font at a pixel size.
ImFont *load_font(const EmbeddedFont &font, float size) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;   // the bytes belong to the program; ImGui must not free them
    config.OversampleH = 2;                // letters drawn twice as finely across: crisper small text
    config.OversampleV = 1;
    // Which characters to prepare: ASCII and Latin-1 (0x20-0xFF), dashes and quotes (0x2010-0x2027),
    // and the triangles (0x25B2-0x25BE). Fewer characters = a smaller texture, faster start.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x25B2, 0x25BE, 0};
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(font.data), font.size, size, &config,
                                                      ranges);
}

// Dear ImGui's own widgets (the right-click menu, tooltips) in the HUD's light look.
void light_style(float scale, ImFont *text) {
    ImGui::StyleColorsLight();   // ImGui's built-in light colours, then our changes
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = style.PopupRounding = style.ScrollbarRounding = style.GrabRounding = 0;   // square corners
    style.PopupBorderSize = 1;
    style.ScrollbarSize = 8 * scale;
    style.WindowPadding = ImVec2(8 * scale, 6 * scale);
    style.ItemSpacing = ImVec2(8 * scale, 4 * scale);
    style.FramePadding = ImVec2(6 * scale, 3 * scale);
    style.Colors[ImGuiCol_PopupBg] = ImColor(colour::PANEL);
    style.Colors[ImGuiCol_Border] = ImColor(colour::BORDER);
    style.Colors[ImGuiCol_Text] = ImColor(colour::TEXT);
    style.Colors[ImGuiCol_TextDisabled] = ImColor(colour::TEXT_QUIET);
    style.Colors[ImGuiCol_HeaderHovered] = ImColor(colour::HOVER);
    style.Colors[ImGuiCol_Separator] = ImColor(colour::FILL);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(colour::FILL);
    style.Colors[ImGuiCol_CheckMark] = ImColor(colour::TEXT);
    ImGui::GetIO().FontDefault = text;
}

}  // namespace

HudView::HudView(TextureStore &textures, double layout_scale, double font_oversample)
    : scale_(static_cast<float>(layout_scale)) {
    const float oversample = static_cast<float>(font_oversample);
    fonts_.text_size = px(12);
    fonts_.text = load_font(INTER_REGULAR, fonts_.text_size * oversample);
    // Big numbers are drawn at many sizes (they follow the window's height). Made once, large
    // (72 design pixels), and shrunk when drawn: shrinking stays sharp, enlarging would blur.
    fonts_.number = load_font(INTER_DISPLAY_LIGHT, px(72) * oversample);
    if (oversample != 1) ImGui::GetIO().FontGlobalScale = 1.0f / oversample;
    light_style(scale_, fonts_.text);
    modules_ = make_modules(fonts_, textures);
    for (const auto &module : modules_) shown_ids_.push_back(module->id());
}

void HudView::apply(const HudSettings &settings) {
    shown_ids_ = settings.modules;
    vertical_ = settings.vertical;
    on_top_ = settings.on_top;
    fps_ = settings.fps;
    opacity_ = settings.opacity;
    sizes_across_ = settings.sizes_across;
    sizes_down_ = settings.sizes_down;
}

void HudView::remember(HudSettings &settings) const {
    settings.modules = shown_ids_;
    settings.vertical = vertical_;
    settings.on_top = on_top_;
    settings.fps = fps_;
    settings.opacity = opacity_;
    settings.sizes_across = sizes_across_;
    settings.sizes_down = sizes_down_;
}

std::vector<Module *> HudView::shown() const {
    std::vector<Module *> out;
    for (const std::string &id : shown_ids_) {
        for (const auto &module : modules_) {
            if (id == module->id()) out.push_back(module.get());   // unknown ids (from an old hud.ini) are skipped
        }
    }
    return out;
}

// Works out each module's box. Along the strip, a module's length is, in this order:
//   1. the length the person dragged it to (sizes(), saved in hud.ini), or
//   2. the module's own fixed length, or
//   3. a share of whatever space is left, by weight ("flexible", like the core heatmap).
// Across the strip, every module gets the full thickness. Edges are rounded to whole pixels so the
// 1-pixel frames stay sharp.
void HudView::lay_out(const std::vector<Module *> &modules, ImVec2 window_size) {
    const float count = static_cast<float>(modules.size());
    const float length = (vertical_ ? window_size.y : window_size.x) - 2 * px(EDGE) - px(GAP) * std::max(0.0f, count - 1);
    const float thickness = (vertical_ ? window_size.x : window_size.y) - 2 * px(EDGE);

    // First pass: the lengths we know, and how much weight shares the rest.
    std::vector<float> lengths(modules.size(), 0);
    float known_total = 0, weight_total = 0;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const std::optional<int> set = size_of(modules[i]->id());
        lengths[i] = set ? px(static_cast<float>(*set)) : modules[i]->fixed_length(vertical_, thickness, scale_);
        known_total += lengths[i];
        if (lengths[i] <= 0) weight_total += modules[i]->weight();
    }
    const float free_space = std::max(0.0f, length - known_total);

    // Second pass: the boxes.
    boxes_.clear();
    flexible_.clear();
    float along = px(EDGE);   // where the next module starts
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const bool flexible = lengths[i] <= 0;
        const float l = flexible ? (weight_total > 0 ? free_space * modules[i]->weight() / weight_total : 0) : lengths[i];
        const float start = std::round(along), end = std::round(along + l);
        boxes_.push_back(vertical_ ? Box{std::round(px(EDGE)), start, std::round(thickness), end - start}
                                   : Box{start, std::round(px(EDGE)), end - start, std::round(thickness)});
        flexible_.push_back(flexible);
        along += l + px(GAP);
    }
}

std::optional<int> HudView::size_of(const std::string &id) const {
    const std::vector<ModuleSize> &list = vertical_ ? sizes_down_ : sizes_across_;
    for (const ModuleSize &size : list) {
        if (size.first == id) return size.second;
    }
    return std::nullopt;
}

void HudView::set_size(const std::string &id, float window_pixels) {
    // Stored in design pixels, so it means the same on any screen scale; never under 30.
    const int design = std::max(30, static_cast<int>(std::lround(window_pixels / scale_)));
    for (ModuleSize &size : sizes()) {
        if (size.first == id) {
            size.second = design;
            return;
        }
    }
    sizes().emplace_back(id, design);
}

void HudView::clear_size(const std::string &id) {
    auto &list = sizes();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const ModuleSize &size) { return size.first == id; }), list.end());
}

ViewRequests HudView::draw(const SystemMonitor &system, const Snapshot &snapshot, double now) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 window_size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled(ImVec2(0, 0), window_size, colour::BACKGROUND);

    const std::vector<Module *> modules = shown();
    lay_out(modules, window_size);
    const HudFrame frame{system, snapshot, now, scale_};
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const Box &box = boxes_[i];
        if (box.w >= 1 && box.h >= 1) modules[i]->draw(draw, box, frame);   // skip boxes squeezed to nothing
    }

    if (modules.empty()) {   // everything turned off: say how to get modules back
        const char *hint = "Right-click to choose modules";
        const float w = text_width(fonts_.text, fonts_.text_size, hint);
        draw->AddText(fonts_.text, fonts_.text_size, ImVec2((window_size.x - w) * 0.5f, (window_size.y - fonts_.text_size) * 0.5f),
                      colour::TEXT_QUIET, hint);
    }
    resize_modules(draw, modules);
    drag_modules(draw);
    return menu();
}

// Resize handles: the gap between two modules. Hover it and the mouse pointer turns into arrows and
// a blue line shows the handle; drag it to make the module before it longer and the one after
// shorter (or the other way round). Double-click it to give both modules back their usual size.
// (MiniMeters has these too: its LayoutChild draws "resize handles", seen with REA.)
//
// Which modules change: a flexible module (like the core heatmap) simply takes whatever space is
// left, so dragging next to one only changes the other module's size. Between two fixed modules,
// one grows by what the other loses.
void HudView::resize_modules(ImDrawList *draw, const std::vector<Module *> &modules) {
    if (boxes_.size() < 2 || dragging_) return;
    const ImGuiIO &io = ImGui::GetIO();
    const float mouse = vertical_ ? io.MousePos.y : io.MousePos.x;    // along the strip
    const float across = vertical_ ? io.MousePos.x : io.MousePos.y;   // across the strip

    // Which handle (gap) is under the mouse, if any. Handle i is between box i and box i + 1.
    std::optional<std::size_t> hovered;
    for (std::size_t i = 0; i + 1 < boxes_.size(); ++i) {
        const Box &a = boxes_[i], &b = boxes_[i + 1];
        const float gap_start = vertical_ ? a.y + a.h : a.x + a.w;
        const float gap_end = vertical_ ? b.y : b.x;
        const float thick_start = vertical_ ? a.x : a.y;
        const float thick_end = thick_start + (vertical_ ? a.w : a.h);
        if (mouse >= gap_start - 1 && mouse <= gap_end + 1 && across >= thick_start && across <= thick_end) hovered = i;
    }
    const auto length_of = [&](std::size_t i) { return vertical_ ? boxes_[i].h : boxes_[i].w; };

    if (!resizing_ && hovered && !ImGui::IsPopupOpen("##hud_menu")) {
        ImGui::SetMouseCursor(vertical_ ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
        const Box &a = boxes_[*hovered];
        const float mid = vertical_ ? a.y + a.h + px(GAP) * 0.5f : a.x + a.w + px(GAP) * 0.5f;
        if (vertical_) draw->AddLine(ImVec2(a.x, mid), ImVec2(a.x + a.w, mid), colour::DRAG, px(2));
        else draw->AddLine(ImVec2(mid, a.y), ImVec2(mid, a.y + a.h), colour::DRAG, px(2));

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // back to the usual sizes
            clear_size(modules[*hovered]->id());
            clear_size(modules[*hovered + 1]->id());
            return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            resizing_ = Resize{*hovered, mouse, length_of(*hovered), length_of(*hovered + 1)};
        }
    }
    if (!resizing_) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || resizing_->handle + 1 >= boxes_.size()) {
        resizing_.reset();   // let go: the sizes are already stored
        return;
    }

    ImGui::SetMouseCursor(vertical_ ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    const std::size_t before = resizing_->handle, after = before + 1;
    const float moved = mouse - resizing_->start_mouse;   // positive = towards the right / bottom
    const float smallest = px(30);
    if (flexible_[before] && !flexible_[after]) {
        set_size(modules[after]->id(), std::max(smallest, resizing_->start_after - moved));
    } else if (!flexible_[before] && flexible_[after]) {
        set_size(modules[before]->id(), std::max(smallest, resizing_->start_before + moved));
    } else {
        // Both fixed (or both flexible): one gains what the other loses, neither below the minimum.
        const float total = resizing_->start_before + resizing_->start_after;
        const float new_before = std::clamp(resizing_->start_before + moved, smallest, std::max(smallest, total - smallest));
        set_size(modules[before]->id(), new_before);
        set_size(modules[after]->id(), total - new_before);
    }
}

// Drag and drop: press on a module and move the mouse a little. The module gets a blue frame, a
// blue line shows where it would land between the others, and letting go puts it there.
void HudView::drag_modules(ImDrawList *draw) {
    const ImGuiIO &io = ImGui::GetIO();

    // Which shown module is at point p, if any.
    const auto module_at = [&](ImVec2 p) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            const Box &b = boxes_[i];
            if (p.x >= b.x && p.x < b.x + b.w && p.y >= b.y && p.y < b.y + b.h) return i;
        }
        return std::nullopt;
    };

    // Start: the left button is held and has moved 6 design pixels (so a plain click isn't a drag),
    // no handle is being resized and the menu isn't open. (Not "no ImGui item active": Dear ImGui
    // marks the whole window active on any click, even one that can't move, so that's always true
    // here; imgui.cpp, StartMouseMovingWindow.)
    if (!dragging_ && !resizing_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, px(6)) &&
        !ImGui::IsPopupOpen("##hud_menu")) {
        dragging_ = module_at(io.MouseClickedPos[ImGuiMouseButton_Left]);   // where the press began
    }
    if (!dragging_ || *dragging_ >= boxes_.size()) {
        dragging_.reset();
        return;
    }

    // Where it would land: in front of the first module whose middle is past the mouse.
    const float mouse = vertical_ ? io.MousePos.y : io.MousePos.x;
    std::size_t slot = boxes_.size();   // default: at the end
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const Box &b = boxes_[i];
        if (mouse < (vertical_ ? b.y + b.h * 0.5f : b.x + b.w * 0.5f)) {
            slot = i;
            break;
        }
    }

    // The blue frame round the dragged module, and the blue landing line.
    const Box &lifted = boxes_[*dragging_];
    draw->AddRect(ImVec2(lifted.x - 1, lifted.y - 1), ImVec2(lifted.x + lifted.w + 1, lifted.y + lifted.h + 1), colour::DRAG,
                  0, 0, px(2));
    const float half_gap = px(GAP) * 0.5f;
    const Box &last = boxes_.back();
    const float line = slot < boxes_.size() ? (vertical_ ? boxes_[slot].y : boxes_[slot].x) - half_gap
                                            : (vertical_ ? last.y + last.h : last.x + last.w) + half_gap;
    const ImVec2 size = io.DisplaySize;
    if (vertical_) draw->AddRectFilled(ImVec2(px(EDGE), line - px(1.5f)), ImVec2(size.x - px(EDGE), line + px(1.5f)), colour::DRAG);
    else draw->AddRectFilled(ImVec2(line - px(1.5f), px(EDGE)), ImVec2(line + px(1.5f), size.y - px(EDGE)), colour::DRAG);
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    // Drop: move the module's id from its old place to the new one.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const std::size_t from = *dragging_;
        // Taking it out first shifts everything after it one place left, hence the -1.
        std::size_t to = slot > from ? slot - 1 : slot;
        const std::string id = shown_ids_[from];
        shown_ids_.erase(shown_ids_.begin() + static_cast<std::ptrdiff_t>(from));
        to = std::min(to, shown_ids_.size());
        shown_ids_.insert(shown_ids_.begin() + static_cast<std::ptrdiff_t>(to), id);
        dragging_.reset();
    }
}

// The right-click menu, anywhere in the window.
ViewRequests HudView::menu() {
    ViewRequests requests;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        ImGui::OpenPopup("##hud_menu");
    }
    if (!ImGui::BeginPopup("##hud_menu")) return requests;   // menu not open: nothing to do

    // One tick-box per module. Turning one on adds it at the end; drag it where it should go.
    ImGui::TextDisabled("Modules");
    for (const auto &module : modules_) {
        const auto found = std::find(shown_ids_.begin(), shown_ids_.end(), module->id());
        bool on = found != shown_ids_.end();
        if (ImGui::MenuItem(module->title(), nullptr, &on)) {
            if (!on) shown_ids_.erase(found);
            else shown_ids_.push_back(module->id());
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Horizontal", nullptr, !vertical_) && vertical_) requests.vertical = vertical_ = false;
    if (ImGui::MenuItem("Vertical", nullptr, vertical_) && !vertical_) requests.vertical = vertical_ = true;
    ImGui::Separator();
    // Frame rate: the screen's own rate is the smoothest; 60 or 30 use less CPU and GPU
    // (measured: about 14 % of a core at 120 frames a second, 6 % at 60).
    if (ImGui::BeginMenu("Frame rate")) {
        struct Choice {
            const char *label;
            int fps;
        };
        for (const Choice &choice : {Choice{"Screen rate (smoothest)", 0}, Choice{"60 fps", 60}, Choice{"30 fps", 30}}) {
            if (ImGui::MenuItem(choice.label, nullptr, fps_ == choice.fps)) requests.fps = fps_ = choice.fps;
        }
        ImGui::EndMenu();
    }
    // Opacity: see the desktop or DAW through the HUD.
    if (ImGui::BeginMenu("Opacity")) {
        for (const int percent : {100, 75, 50, 25}) {
            const std::string label = std::to_string(percent) + " %";
            if (ImGui::MenuItem(label.c_str(), nullptr, opacity_ == percent)) requests.opacity = opacity_ = percent;
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Keep on top", nullptr, on_top_)) requests.keep_on_top = on_top_ = !on_top_;
    if (ImGui::MenuItem("Reset layout")) {   // the default modules, order and sizes
        shown_ids_ = HudSettings{}.modules;
        sizes_across_.clear();
        sizes_down_.clear();
    }
    ImGui::EndPopup();
    return requests;
}

}  // namespace sessionscope
