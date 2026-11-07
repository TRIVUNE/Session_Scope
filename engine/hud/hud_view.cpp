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
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(font.data), font.size, size, &config);
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
    style.Colors[ImGuiCol_HeaderHovered] = ImColor(colour::ROW_NOW);
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

ViewRequests HudView::draw(const CpuMonitor &cpu, const Snapshot &snapshot, double now) {
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

    const HudFrame frame{cpu, snapshot, now, scale_};
    float along = pad;
    for (Module *module : modules) {
        float l = module->fixed_length(vertical_, thickness, scale_);
        if (l <= 0) l = weights > 0 ? flexible * module->weight() / weights : 0;
        const float start = std::round(along), end = std::round(along + l);
        const Box box = vertical_ ? Box{std::round(pad), start, std::round(thickness), end - start}
                                  : Box{start, std::round(pad), end - start, std::round(thickness)};
        if (box.w >= 1 && box.h >= 1) module->draw(draw, box, frame);
        along += l + gap;
    }
    if (modules.empty()) {
        const char *hint = "Right-click to choose modules";
        draw->AddText(fonts_.text, fonts_.text_size,
                      ImVec2((size.x - text_width(fonts_.text, fonts_.text_size, hint)) * 0.5f, (size.y - fonts_.text_size) * 0.5f),
                      colour::TEXT_QUIET, hint);
    }
    return menu();
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
            if (!on) {
                shown_ids_.erase(found);
            } else {
                // Back in its usual place among the modules shown.
                std::vector<std::string> ordered;
                for (const auto &m : modules_) {
                    const std::string id = m->id();
                    if (id == module->id() || std::find(shown_ids_.begin(), shown_ids_.end(), id) != shown_ids_.end()) {
                        ordered.push_back(id);
                    }
                }
                shown_ids_ = ordered;
            }
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Horizontal", nullptr, !vertical_) && vertical_) requests.vertical = vertical_ = false;
    if (ImGui::MenuItem("Vertical", nullptr, vertical_) && !vertical_) requests.vertical = vertical_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Keep on top", nullptr, on_top_)) requests.keep_on_top = on_top_ = !on_top_;
    ImGui::EndPopup();
    return requests;
}

}  // namespace sessionscope
