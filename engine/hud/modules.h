#ifndef SESSIONSCOPE_HUD_MODULES_H
#define SESSIONSCOPE_HUD_MODULES_H

// The HUD is a row (or column) of modules, as in MiniMeters: each shows one thing, and the person
// picks which ones and in which direction they run (right-click menu, hud_view.h). Each module
// draws inside the box it is given and adapts to its shape: the peak meter fills upward in a tall
// box and rightward in a wide one.
//
//   meter  peak meter of the DAW's CPU             (fixed thickness)
//   cpu    the DAW's CPU share, as a big number    (fixed: as wide as "100%")
//   cores  heatmap of every core, with bar lines   (shares the rest)
//   bars   list of the song's bars and how late    (shares the rest)
//          their blocks ran: the Task Manager view

#include <memory>
#include <vector>

#include "cpu_monitor.h"
#include "hud_engine.h"
#include "renderer.h"
#include "theme.h"

struct ImDrawList;

namespace sessionscope {

// Everything a module may draw from, for one display frame.
struct HudFrame {
    const CpuMonitor &cpu;
    const Snapshot &snapshot;   // the newest engine snapshot (bars, transport)
    double now;                 // seconds, on the CpuMonitor's clock
    float scale;                // window units per CSS pixel
};

struct Box {
    float x, y, w, h;
};

class Module {
public:
    virtual ~Module() = default;
    // Saved in hud.ini; never shown.
    virtual const char *id() const = 0;
    // Shown in the right-click menu.
    virtual const char *title() const = 0;
    // The module's length along the strip, in window units, when the strip is `thickness` across;
    // 0 to share the space left over with the other such modules, in proportion to weight().
    virtual float fixed_length(bool vertical, float thickness, float scale) const = 0;
    virtual float weight() const { return 1; }
    virtual void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) = 0;
};

// Every module, in their default order.
std::vector<std::unique_ptr<Module>> make_modules(const Fonts &fonts, TextureStore &textures);

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_MODULES_H
