# Handoff: where the HUD is going

For the next AI agent working on this repository. Read this first, then `README.md` and `guide.md`.
Last updated 2026-11-07 (evening).

## The direction

The SessionScope HUD is becoming a **MiniMeters-style strip of modules** for watching a DAW session's
performance: one long window, horizontal or vertical, where the person picks which modules to show
and drags them into the order they want.

- **Modular.** Every view is a `Module` (`engine/hud/modules/module.h`). New views are new modules, never
  new fixed regions of the window. Modules adapt to the box they get (tall or wide).
- **Smooth.** On Windows the HUD draws like MiniMeters: Direct3D 11 into a DirectComposition flip
  swap chain, one frame queued, waiting on the frame-latency waitable object
  (`engine/hud/window/renderer_d3d11.cpp`, on the GPU that drives the window's screen). Things that move are placed by time and slide every display
  frame; data arrives ten times a second. macOS keeps OpenGL (`window/renderer_gl.cpp`, untested lately).
- **Look.** A plain light plug-in panel: pale grey background, white panels with thin dark borders,
  Inter (Display Light for big numbers, Regular for text), no branding or decoration
  (`engine/hud/look/theme.h`). Heatmaps use the "jet" palette: dark blue, blue, cyan, green, yellow,
  red. Red text only for values that mean trouble.
- **Only what helps a session.** The person strips anything that isn't useful: no status lines,
  no labels for their own sake, no logos.
- **Native and fast.** C++ only at run time, readable code with plain comments. The build is
  moving from Python to CMake (`cmake-move.md`).

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

1. **Task Manager: the Probe plug-in (phase 2 of `task-manager-plan.md`).** Phase 1 is done: the
   probes' shared table, the reader and the module work, shown in `--demo` by simulated probes.
   Next is the SessionScope Probe VST3 plug-in that loads a real plug-in and times it; then the
   CLAP probe and track names from the host (phase 3). The full to-do list is in `STATUS.md`.
2. **Build with CMake instead of Python, phase 3** (the person's decision): phases 1 and 2 are
   done (CMake builds and tests everything); phase 3 (installing, the REAPER tools, setup, deleting
   Python, docs) is **not started: the person said to stop before it. Ask before starting.**
   Everything you need is in `cmake-move.md`.
3. **The big check, 4 passes** (the person's request): see `STATUS.md`.
4. **macOS:** the tray icon, hotkey, taskbar choice and native menus are Windows-only so far
   (`window/desktop.cpp`, `window/menu.cpp` have empty stand-ins); nothing on macOS was built or
   run in this round.

Done from MiniMeters' second REA pass (`minimeters.md`), all chosen by the person: resize handles
between modules, pop-out windows, frame-rate limit, window opacity, tray icon, hide from taskbar,
show/hide hotkey, and real Windows right-click menus (an ImGui menu was cut off by the thin strip).
Not visually confirmed yet: the tray icon's picture (its menu item, setting and message window work).

Code map for beginners: `engine/hud/README.md`. Every file starts with what it does and where its
ideas and system calls come from; keep it that way (the person asked for comments everywhere,
because the team are beginners).

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
- **Check what you build:** `cmake --build --preset windows`, then `ctest --preset windows` (every
  test must pass; warnings are errors in our code) and, after plug-in changes,
  `ctest --preset windows-plugins`. Then look at the HUD with
  `build/engine/SessionScopeHUD.exe --demo --screenshot out.png --wait 9000` (add `--vertical`
  for the column layout).
- The demo has no DAW, so its DAW CPU is a stand-in; say so wherever it matters.
- **Never touch the person's own `hud.ini`** (`%APPDATA%\SessionScope`) in tests: pass
  `--settings <scratch file>`; screenshot runs don't read or write settings at all. Their settings
  were lost once this way.
- **Test drags with `--test-drag X1,Y1,X2,Y2`**, not a script-moved mouse: this screen runs at
  200 %, and Windows rescales a non-DPI-aware script's mouse, so it can't aim.
- **Shell edits mangle backslashes** in this environment's bash heredocs (`\n`, `\x`, `\;`): edit
  files with the Edit/Write tools when the text has backslashes.
