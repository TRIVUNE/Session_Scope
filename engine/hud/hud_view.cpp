// The HUD's layout and menu: see hud_view.h.

#include "hud_view.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "fonts_data.h"

namespace sessionscope {

namespace {

// A font compiled into the program (fonts_data.h), at a pixel size. The atlas only borrows the
// bytes: they live for the whole run.
ImFont *load_font(const EmbeddedFont &font, float size) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 2;
    config.OversampleV = 1;
    // Latin, plus dashes and quotes (U+2010..U+2027) and the menu's triangles.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x25B2, 0x25BE, 0};
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(font.data), font.size, size, &config,
                                                      ranges);
}

// ImGui's own widgets (the menu, the list's scroll bar) in the HUD's light look.
void light_style(float scale, ImFont *text) {
    ImGui::StyleColorsLight();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = style.PopupRounding = style.ScrollbarRounding = style.GrabRounding = 0;
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
    // The number is drawn at many sizes (it follows the window's height): rasterised large and
    // scaled down, which stays sharp.
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
}

void HudView::remember(HudSettings &settings) const {
    settings.modules = shown_ids_;
    settings.vertical = vertical_;
    settings.on_top = on_top_;
}

std::vector<Module *> HudView::shown() const {
    std::vector<Module *> out;
    for (const std::string &id : shown_ids_) {
        for (const auto &module : modules_) {
            if (id == module->id()) out.push_back(module.get());
        }
    }
    return out;
}

ViewRequests HudView::draw(const SystemMonitor &system, const Snapshot &snapshot, double now) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled(ImVec2(0, 0), size, colour::BACKGROUND);

    // Lay the modules out along the strip: fixed ones take their length, the rest share what is
    // left by weight. Edges land on whole pixels so the one-pixel frames stay sharp.
    const std::vector<Module *> modules = shown();
    const float pad = px(8), gap = px(8);
    const float length = (vertical_ ? size.y : size.x) - 2 * pad - gap * std::max<int>(0, static_cast<int>(modules.size()) - 1);
    const float thickness = (vertical_ ? size.x : size.y) - 2 * pad;
    float fixed = 0, weights = 0;
    for (const Module *module : modules) {
        const float l = module->fixed_length(vertical_, thickness, scale_);
        fixed += l;
        if (l <= 0) weights += module->weight();
    }
    const float flexible = std::max(0.0f, length - fixed);

    const HudFrame frame{system, snapshot, now, scale_};
    float along = pad;
    boxes_.clear();
    for (Module *module : modules) {
        float l = module->fixed_length(vertical_, thickness, scale_);
        if (l <= 0) l = weights > 0 ? flexible * module->weight() / weights : 0;
        const float start = std::round(along), end = std::round(along + l);
        const Box box = vertical_ ? Box{std::round(pad), start, std::round(thickness), end - start}
                                  : Box{start, std::round(pad), end - start, std::round(thickness)};
        boxes_.push_back(box);
        if (box.w >= 1 && box.h >= 1) module->draw(draw, box, frame);
        along += l + gap;
    }
    if (modules.empty()) {
        const char *hint = "Right-click to choose modules";
        draw->AddText(fonts_.text, fonts_.text_size,
                      ImVec2((size.x - text_width(fonts_.text, fonts_.text_size, hint)) * 0.5f, (size.y - fonts_.text_size) * 0.5f),
                      colour::TEXT_QUIET, hint);
    }
    drag_modules(draw);
    return menu();
}

// Press on a module and move the mouse: it lifts (blue frame), a blue line shows where it will
// land between the others, and letting go puts it there.
void HudView::drag_modules(ImDrawList *draw) {
    const ImGuiIO &io = ImGui::GetIO();
    const auto hit = [&](ImVec2 p) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < boxes_.size(); ++i) {
            const Box &b = boxes_[i];
            if (p.x >= b.x && p.x < b.x + b.w && p.y >= b.y && p.y < b.y + b.h) return i;
        }
        return std::nullopt;
    };
    if (!dragging_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, px(6)) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("##hud_menu")) {
        dragging_ = hit(io.MouseClickedPos[ImGuiMouseButton_Left]);
    }
    if (!dragging_ || *dragging_ >= boxes_.size()) {
        dragging_.reset();
        return;
    }

    // Where it would land: before the first module whose middle is past the mouse.
    const float mouse = vertical_ ? io.MousePos.y : io.MousePos.x;
    std::size_t slot = boxes_.size();
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const Box &b = boxes_[i];
        if (mouse < (vertical_ ? b.y + b.h * 0.5f : b.x + b.w * 0.5f)) {
            slot = i;
            break;
        }
    }

    const Box &lifted = boxes_[*dragging_];
    draw->AddRect(ImVec2(lifted.x - 1, lifted.y - 1), ImVec2(lifted.x + lifted.w + 1, lifted.y + lifted.h + 1), colour::DRAG, 0,
                  0, px(2));
    const float half_gap = px(4);
    float line;
    if (slot < boxes_.size()) line = (vertical_ ? boxes_[slot].y : boxes_[slot].x) - half_gap;
    else line = vertical_ ? boxes_.back().y + boxes_.back().h + half_gap : boxes_.back().x + boxes_.back().w + half_gap;
    const ImVec2 size = io.DisplaySize;
    if (vertical_) draw->AddRectFilled(ImVec2(px(8), line - px(1.5f)), ImVec2(size.x - px(8), line + px(1.5f)), colour::DRAG);
    else draw->AddRectFilled(ImVec2(line - px(1.5f), px(8)), ImVec2(line + px(1.5f), size.y - px(8)), colour::DRAG);
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const std::size_t from = *dragging_;
        std::size_t to = slot > from ? slot - 1 : slot;
        const std::string id = shown_ids_[from];
        shown_ids_.erase(shown_ids_.begin() + static_cast<std::ptrdiff_t>(from));
        to = std::min(to, shown_ids_.size());
        shown_ids_.insert(shown_ids_.begin() + static_cast<std::ptrdiff_t>(to), id);
        dragging_.reset();
    }
}

// Right-click anywhere: tick the modules to show, pick the direction, keep the window on top.
ViewRequests HudView::menu() {
    ViewRequests requests;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        ImGui::OpenPopup("##hud_menu");
    }
    if (!ImGui::BeginPopup("##hud_menu")) return requests;

    ImGui::TextDisabled("Modules");
    for (const auto &module : modules_) {
        const auto found = std::find(shown_ids_.begin(), shown_ids_.end(), module->id());
        bool on = found != shown_ids_.end();
        if (ImGui::MenuItem(module->title(), nullptr, &on)) {
            if (!on) shown_ids_.erase(found);
            else shown_ids_.push_back(module->id());   // at the end; drag it where it should go
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Horizontal", nullptr, !vertical_) && vertical_) requests.vertical = vertical_ = false;
    if (ImGui::MenuItem("Vertical", nullptr, vertical_) && !vertical_) requests.vertical = vertical_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Keep on top", nullptr, on_top_)) requests.keep_on_top = on_top_ = !on_top_;
    if (ImGui::MenuItem("Reset layout")) shown_ids_ = HudSettings{}.modules;
    ImGui::EndPopup();
    return requests;
}

}  // namespace sessionscope
