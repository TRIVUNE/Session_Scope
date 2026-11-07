#ifndef SESSIONSCOPE_HUD_MODULE_H
#define SESSIONSCOPE_HUD_MODULE_H

// =================================================================================================
// module.h - what every HUD module is
// =================================================================================================
//
// The HUD is a strip of modules, side by side (or stacked when vertical). Each module shows one
// thing. The person picks which ones from the right-click menu and drags them into any order
// (hud_view.cpp does that part). The modules don't know where they are: each frame they are handed
// a box and draw inside it, adapting to its shape (tall or wide).
//
// Where the idea comes from: MiniMeters, which is built the same way. Reading MiniMeters.exe with
// REA showed classes named Module, SingleModule and LayoutManager, and a settings file storing
// per module "enabled", "order", "size" and "popout.enabled" (see handoff/minimeters.md).
//
// The modules, one file each in this folder:
//
//   id         file                 shows
//   meter      peak_meter.cpp       the DAW's CPU as a bar, with a peak mark
//   cpu        cpu_number.cpp       the DAW's CPU as a big number
//   cores      core_heatmap.cpp     every core of the machine over time, with the song's bars
//   busiest    busiest_core.cpp     the single busiest core
//   dropouts   dropouts.cpp         audio blocks late enough to drop out, and the headroom left
//   memory     memory_disk.cpp      the DAW's RAM, free RAM, disk reads
//   clock      cpu_clock_card.cpp   the CPU's speed, and whether Windows is slowing it down
//   tasks      task_manager.cpp     CPU per track and per plug-in, from the SessionScope Probes
//
// To add a module: copy the smallest one (cpu_number.cpp), change it, give it a new id, declare its
// make_... function in module_list.h and add it to make_modules() in module_list.cpp.

#include "hud_engine.h"       // Snapshot: what the engine measured from the sensor (lateness, transport)
#include "look/theme.h"       // colours and fonts
#include "probe_monitor.h"    // ProbeMonitor: CPU per track and plug-in, from the Probes
#include "system_monitor.h"   // SystemMonitor: readings from this computer (cores, memory, clock)

struct ImDrawList;   // Dear ImGui's list of things to draw this frame

namespace sessionscope {

// Everything a module may draw from, for one frame.
struct HudFrame {
    const SystemMonitor &system;   // the computer's readings, the last 12 seconds
    const Snapshot &snapshot;      // the engine's newest snapshot (transport, lateness, dropouts)
    const ProbeMonitor &probes;    // CPU per track and plug-in (the Task Manager)
    double now;                    // the time now, in seconds, on the SystemMonitor's clock
    float scale;                   // window pixels per design pixel: 1.5 on a screen set to 150 %
};

// A rectangle on screen, in window pixels: left edge x, top edge y, width w, height h.
struct Box {
    float x, y, w, h;
};

class Module {
public:
    virtual ~Module() = default;

    // A short name saved in hud.ini, never shown ("meter", "cores", ...).
    virtual const char *id() const = 0;
    // The name shown in the right-click menu.
    virtual const char *title() const = 0;
    // How long the module wants to be along the strip, in window pixels, when the strip is
    // `thickness` pixels across. Return 0 to share whatever space is left with the other modules
    // that return 0, in proportion to weight().
    virtual float fixed_length(bool vertical, float thickness, float scale) const = 0;
    virtual float weight() const { return 1; }
    // Draws the module inside `box`. Called once per frame (120 times a second on a 120 Hz screen),
    // so keep it light: no file reading, no big memory allocations.
    virtual void draw(ImDrawList *draw, const Box &box, const HudFrame &frame) = 0;
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_HUD_MODULE_H
