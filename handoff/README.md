# Handoff: where the HUD is going

For the next AI agent working on this repository. Read this first, then `README.md` and `guide.md`.
Last updated 2026-11-12.

## The project is paused (12 Nov 2026, evening)

**SessionScope stopped as a product.** Decision record: **`docs/decisions/0007-stop.md`**; the
reasoning and every failure on the way: **`docs/what-failed.md`**. In short: the person's four
requirements (no DAW dependence, no carrying settings over, nothing of ours inside the DAW, more
than the DAW already shows) cannot all be met, because per-instance plug-in numbers in a running
session exist only inside the DAW. Every approach built or discussed met at most three.

**Do not resume product work** (calibration, more project readers, the sampler, the HUD
re-targeting) unless the person reopens the question. Nothing was deleted.

**Reusable parts**, each built, tested and documented:

| Part | Where | What it does |
| --- | --- | --- |
| Offline bench | `native/bench/bench_host.cpp`, `engine/src/bench_runner.*`, `sessionscope bench` | Hosts any VST3 plug-in out of process (job object, watchdog, crash and hang reports), times every block, checks the plug-in really processes, loads factory programs, `.vstpreset`s or saved states, finds plug-ins by id (cached scan) |
| Project state readers | `engine/src/rpp_reader.*`, `als_reader.*`, `inflate.*` | Every VST3 plug-in's saved state out of a REAPER project or an Ableton Live set; proven exact (`docs/research/rpp-state.md`, `als-state.md`) |
| HUD engine | `engine/hud/` | Direct3D 11 + DirectComposition strip of modules, MiniMeters-smooth |
| Sensors, extension, tests | `native/`, `hosts/reaper/` | VST3/CLAP/AU sensors, the REAPER real-time CPU extension, validators, an isolated-REAPER test harness |

Everything below describes the project as it was before the stop.

## The direction changed on 12 Nov 2026

The person tested the plug-ins in Ableton Live for the first time: ten of ten hosted effects
crashed Live, no instrument Probe exists, and the state-migration workflow was named a
dealbreaker. The product direction is now the **offline session analyzer** — read the project
file, measure every plug-in in SessionScope's own offline host (separate process, the plug-in's
own saved state, the project's own MIDI and audio), report ranked costs and visuals. Decision
record: **`docs/decisions/0006-offline-session-analyzer.md`**; the evening's findings:
`docs/research/Live-first-look.md`; prior-art scouting and the DRM defense:
`docs/research/offline-analyzer-scouting.md`. What failed on the way here, and why, in one
place: **`docs/what-failed.md`** (read it before proposing an idea again). Two consequences for the code in this handoff:

- **The HUD is re-targeted as the analyzer app's interface.** Its live-monitor role (watching
  a running DAW) is retired; its engine — the Direct3D 11 rendering, the module framework,
  settings, and trace replay support — is kept and becomes the app's HUD: the weight map, the
  suspects and waste lists, trace playback, the what-if comparisons. The S2 Dropouts fix
  (a)+(b) is paused with the live monitor; revisit if it ever returns.
- **The Probe is triaged to the code the app needs.** The offline hosting core
  (`native/vst3_probe/hosted_vst3.*`) and the per-block timing it already carries migrate from
  "VST plug-in" to the app: the same hosting and timing, now inside `sessionscope bench`. The
  VST3 plug-in shell (picker, editor), the CLAP probe and twins, the REAPER *Probe all FX*
  scripts and the sensor retire from the product and the build (kept in the tree and history
  as research).

**Built so far for the analyzer (12 Nov 2026):** `sessionscope bench PLUGIN.vst3...` runs each
plug-in in its own bench host process (`native/bench/bench_host.cpp`, runner
`engine/src/bench_runner.*`), with a watchdog and the engagement check. First gauntlet pass at
default settings: 9 of 10 run headless (`docs/research/gauntlet.md`). The person's presets are
in vendor formats no host can open, so settings come from projects instead:
`sessionscope bench --project FILE.rpp` reads every VST3 plug-in's saved state out of a REAPER
project (`engine/src/rpp_reader.*`); all 13,455 parameters of ten plug-ins matched REAPER after
reopening (`docs/research/rpp-state.md`). Ableton Live sets the same way (`--project FILE.als`, `engine/src/als_reader.*`, `inflate.*`; `docs/research/als-state.md`). Next: calibration against REAPER's live numbers.

The rest of this handoff still describes the HUD as it is today; read it as the engine that
will be re-pointed at the analyzer's data.

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
  CMake (`cmake-move.md`); installing and the REAPER tools are commands of the console program.

## Done

| Module | Source of data |
| --- | --- |
| Peak meter, CPU number | the DAW process's CPU (`ProcessCpu` on the sensor's host process id) |
| Core heatmap | `CoreCpu`: every logical core (NtQuerySystemInformation / host_processor_info) |
| Busiest core | the same readings |
| Dropouts | engine snapshot: blocks past the lateness "overrun" tier, headroom from late p99 |
| Memory and disk | `ProcessResources` (DAW working set, bytes read) and `system_memory()` |
| CPU clock | `CpuClock`: Windows Processor Information counters via pdh.dll, loaded at run time |
| Task Manager (CPU per track and plug-in) | `ProbeMonitor`: the SessionScope Probes' shared table (`task-manager-plan.md`) |

All machine readings go through `SystemMonitor` (`engine/src/system_monitor.h`), kept apart from
`HudEngine` so engine snapshots stay reproducible from a trace. Settings (modules shown and order,
direction, on top, window place) are in `hud.ini` (`engine/src/hud_settings.h`).

## Next, in order

0. **Decide how the HUD detects a dropout** (the person's decision; found on 11 Nov 2026 by S2
   on the real interface). The Komplete Audio's ASIO driver spaces its callbacks 4, 4, 8 ms, so
   every third block is "50 % late" and the Dropouts module counted 749 dropouts in a session
   at 2 % load. The lateness measure also does not follow load on real hardware. The numbers and
   three ways to fix it are in `docs/research/S2-lateness.md`. Offer them; build what is picked.
1. **The Probes, by hand in more DAWs.** The Task Manager is built end to end (all three phases
   of `task-manager-plan.md`): the VST3 Probe, the CLAP Probe, track names from the host, the
   REAPER scripts that wrap every effect on a track, `sessionscope probes`. It was checked in a
   real REAPER with third-party plug-ins, the picker window and the CLAP twins included. Not
   yet done, because it needs a person in the DAW: Cubase, Ableton Live, Bitwig. The list is
   near the end of `task-manager-plan.md`.
2. **The big check, pass 4 (live)** and the rest of the person's P1 list in `STATUS.md` (the audio
   interface, spike E, spike C with real plug-ins): these need the person at the computer.
3. **Gate G1**: the decision record. The Probe makes per-plug-in CPU available in any VST3 host,
   which was the missing piece for path A.
4. **macOS:** the tray icon, hotkey, taskbar choice and native menus are Windows-only so far
   (`window/desktop.cpp`, `window/menu.cpp` have empty stand-ins); the Probes are Windows-only;
   nothing on macOS was built or run in the last two rounds.

Done on 11 Nov 2026: the CMake move's phase 3 (the person said to go ahead; no Python is left:
`cmake-move.md`), Task Manager phases 2 and 3, scrolling in the Task Manager, big-check
passes 1 to 3 and the part of pass 4 that needs no person (what they found is in `STATUS.md`).
The person's instruction for that round was "finish this project", narrowed to "everything
which you can test on this machine: we have Ableton, Cubase and REAPER".

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
  `strings`-style dumps: the person explicitly asked for REA. Setup on the person's earlier
  computer: portable Node 24.22.2, Temurin JDK 21 and Ghidra 12.1.4 in `C:\tools` (REA rejects
  Ghidra 12.2). The current computer (since 11 Nov 2026) has no `C:\tools`: CMake and Ninja come
  from scoop there, and REA's own tools should be checked with `rea doctor` before relying on them. Large
  executables need a longer Ghidra startup: call `update_analysis_settings` with
  `ghidra.startup_timeout_ms` set to 1800000 before the first query. What REA found in
  MiniMeters is in `minimeters.md` in this folder.
- **Do the work yourself.** The person does not want work handed to OpenCode workers or other
  agents in this project.
- **Ask before building** something new: offer options, build only what is picked.
- **Commit after each phase**, with a plain message saying what changed and why.
- **Check what you build:** `cmake --build --preset windows`, then `ctest --preset windows` (every
  test must pass; warnings are errors in our code) and, after plug-in changes,
  `ctest --preset windows-plugins` (close any DAW first), and after changes to the REAPER
  scripts or the Probe, `build/engine/sessionscope reaper validate` (a real REAPER with its own
  settings folder, about five minutes; nothing may rebuild while it runs). Then look at the HUD with
  `build/engine/SessionScopeHUD.exe --demo --screenshot out.png --wait 9000` (add `--vertical`
  for the column layout).
- The demo has no DAW, so its DAW CPU is a stand-in; say so wherever it matters.
- **Testing inside a real REAPER without touching the person's own:** start it on the
  validation's settings folder with a Lua script to run:
  `reaper.exe -cfgfile build\reaper-validate\reaper.ini -newinst -nosplash -new script.lua`.
  The script can build a project, play it, call `sessionscope probes` through
  `reaper.ExecProcess`, write what it finds to a file and quit (action 40004). A window's
  controls can be pressed from outside by sending them messages (`BM_CLICK`, `LB_SELECTSTRING`),
  which is how the Probe's picker was tested. On the dummy audio device the lateness numbers
  mean nothing (it does not keep real time).
- **Never touch the person's own `hud.ini`** (`%APPDATA%\SessionScope`) in tests: pass
  `--settings <scratch file>`; screenshot runs don't read or write settings at all. Their settings
  were lost once this way.
- **Test drags with `--test-drag X1,Y1,X2,Y2`** (a click is a drag that goes nowhere) and the
  mouse wheel with `--test-wheel X,Y,NOTCHES`, not a script-moved mouse: this screen runs at
  200 %, and Windows rescales a non-DPI-aware script's mouse, so it can't aim.
- **Shell edits mangle backslashes** in this environment's bash heredocs (`\n`, `\x`, `\;`): edit
  files with the Edit/Write tools when the text has backslashes.
