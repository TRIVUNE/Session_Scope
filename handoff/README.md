# Handoff: where the HUD is going

For the next AI agent working on this repository. Read this first, then `README.md` and `guide.md`.
Last updated 2026-11-07.

## The direction

The SessionScope HUD is becoming a **MiniMeters-style strip of modules** for watching a DAW session's
performance: one long window, horizontal or vertical, where the person picks which modules to show
and drags them into the order they want.

- **Modular.** Every view is a `Module` (`engine/hud/modules.h`). New views are new modules, never
  new fixed regions of the window. Modules adapt to the box they get (tall or wide).
- **Smooth.** On Windows the HUD draws like MiniMeters: Direct3D 11 into a DirectComposition flip
  swap chain, one frame queued, waiting on the frame-latency waitable object
  (`engine/hud/renderer_d3d11.cpp`). Things that move are placed by time and slide every display
  frame; data arrives ten times a second. macOS keeps OpenGL (`renderer_gl.cpp`, untested lately).
- **Look.** A plain light plug-in panel: pale grey background, white panels with thin dark borders,
  Inter (Display Light for big numbers, Regular for text), no branding or decoration
  (`engine/hud/theme.h`). Heatmaps use the "jet" palette: dark blue, blue, cyan, green, yellow,
  red. Red text only for values that mean trouble.
- **Only what helps a session.** The person strips anything that isn't useful: no status lines,
  no labels for their own sake, no logos.
- **Native and fast.** C++ only at run time, readable code with plain comments. Python is used only
  to build (an open question: see below).

## Done

| Module | Source of data |
| --- | --- |
| Peak meter, CPU number | the DAW process's CPU (`ProcessCpu` on the sensor's host process id) |
| Core heatmap | `CoreCpu`: every logical core (NtQuerySystemInformation / host_processor_info) |
| Busiest core | the same readings |
| Dropouts | engine snapshot: blocks past the lateness "overrun" tier, headroom from late p99 |
| Memory and disk | `ProcessResources` (DAW working set, bytes read) and `system_memory()` |
| CPU clock | `CpuClock`: Windows Processor Information counters via pdh.dll, loaded at run time |

All machine readings go through `SystemMonitor` (`engine/src/system_monitor.h`), kept apart from
`HudEngine` so engine snapshots stay reproducible from a trace. Settings (modules shown and order,
direction, on top, window place) are in `hud.ini` (`engine/src/hud_settings.h`).

## Next, in order

1. **Task Manager module with CPU per track and per plug-in.** The person chose a **wrapper
   plug-in**: a "SessionScope Probe" VST3/CLAP that loads the real plug-in inside it, times each
   of its blocks, and publishes the cost through shared memory with its track name. The HUD
   module lists tracks, each expandable into its plug-ins, with each one's CPU. This is the main
   missing piece and the biggest build (it means hosting VST3 and CLAP plug-ins inside ours,
   passing through parameters, editor, latency and state). Plan it, show the plan, then build it.
2. **Pop a module out into its own window** (MiniMeters lets you detach modules).
3. **The HTML prototype** (`design/hud-prototype/index.html`) still shows the old black
   spectrogram design. Bring it in line with the strip, or delete it if the person agrees.
4. **Open question, ask before acting:** the person asked about removing Python. Python only
   builds and installs (16 scripts in `tools/` and `hosts/reaper/tools/`); nothing runs it at run
   time. Options offered: keep it, CMake, or Zig's build system. No answer yet.

The "Bars list" module (lateness per bar of the song) was removed at the person's request; the
engine still computes the bar chart for `sessionscope report`.

## How to work here

- **Use REA for anything about a shipped binary.** REA (github.com/morluto/rea) is installed as the
  `rea` MCP server, with the `reverse-engineer-anything` skill. To learn how another program does
  something (MiniMeters, a DAW, a plug-in), open its executable with REA and Ghidra, not with
  `strings`-style dumps: the person explicitly asked for REA. Setup on this machine: portable Node
  24.22.2, Temurin JDK 21 and Ghidra 12.1.4 in `C:\tools` (REA rejects Ghidra 12.2). Large
  executables need a longer Ghidra startup: call `update_analysis_settings` with
  `ghidra.startup_timeout_ms` set to 1800000 before the first query. What REA found in
  MiniMeters is in `minimeters.md` in this folder.
- **Do the work yourself.** The person does not want work handed to OpenCode workers or other
  agents in this project.
- **Ask before building** something new: offer options, build only what is picked.
- **Commit after each phase**, with a plain message saying what changed and why.
- **Check what you build:** `python tools/build_engine.py --test` (every test must pass, warnings
  are errors), then look at the HUD with
  `build/engine/SessionScopeHUD.exe --demo --screenshot out.png --wait 9000` (add `--vertical`
  for the column layout).
- The demo has no DAW, so its DAW CPU is a stand-in; say so wherever it matters.
