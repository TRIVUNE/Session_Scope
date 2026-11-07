#ifndef SESSIONSCOPE_HUD_MODULE_LIST_H
#define SESSIONSCOPE_HUD_MODULE_LIST_H

// =================================================================================================
// module_list.h - every module the HUD has
// =================================================================================================
//
// Each module lives in its own .cpp file and is only reachable through its make_... function, so
// the files don't need to know about each other. make_modules() (module_list.cpp) calls them all.

#include <memory>
#include <vector>

#include "look/theme.h"
#include "modules/module.h"
#include "window/renderer.h"

namespace sessionscope {

// One function per module, each defined in the module's own file.
std::unique_ptr<Module> make_peak_meter();
std::unique_ptr<Module> make_cpu_number(const Fonts &fonts);
std::unique_ptr<Module> make_core_heatmap(const Fonts &fonts, TextureStore &textures);
std::unique_ptr<Module> make_busiest_core(const Fonts &fonts);
std::unique_ptr<Module> make_dropouts(const Fonts &fonts);
std::unique_ptr<Module> make_memory_disk(const Fonts &fonts);
std::unique_ptr<Module> make_cpu_clock_card(const Fonts &fonts);

// Every module, in their default order (the order of HudSettings::modules in hud_settings.h).
std::vector<std::unique_ptr<Module>> make_modules(const Fonts &fonts, TextureStore &textures);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_MODULE_LIST_H
