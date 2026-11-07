// =================================================================================================
// module_list.cpp - makes every module, in their default order (see module_list.h)
// =================================================================================================

#include "modules/module_list.h"

namespace sessionscope {

std::vector<std::unique_ptr<Module>> make_modules(const Fonts &fonts, TextureStore &textures) {
    std::vector<std::unique_ptr<Module>> modules;
    modules.push_back(make_peak_meter());
    modules.push_back(make_cpu_number(fonts));
    modules.push_back(make_core_heatmap(fonts, textures));
    modules.push_back(make_busiest_core(fonts));
    modules.push_back(make_dropouts(fonts));
    modules.push_back(make_memory_disk(fonts));
    modules.push_back(make_cpu_clock_card(fonts));
    return modules;
}

}  // namespace sessionscope
