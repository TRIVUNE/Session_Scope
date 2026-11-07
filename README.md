# SessionScope

Performance diagnostics for digital audio workstations, by TRIVUNE: find where in the song a s
session chokes, understand why, test a fix, and see whether it helped.

It works with any DAW that loads VST3, CLAP or (on macOS) Audio Unit plug-ins. A small sensor
plug-in on the master track measures every audio block; a separate HUD window, always on top,
shows how late the blocks run and in which bars of the song. REAPER gets extra data through an
optional add-on.

**Status: P1, proof of concept.** This repository holds the measurement research the blueprint
(edition 1.1, 27 Oct 2026) puts before any product work: what a host lets an outside tool measure,
and whether those measurements mean anything. Most of that research was done in REAPER. Nothing
here is a finished product, and numbers from simulated traces are labelled as such everywhere
they appear. Why the code looks the way it does: [decision 0005](docs/decisions/0005-daw-neutral-native.md).

## What is here

| Path            | What it is                                                                                                                                                                                                                                                     |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `native/`       | the sensor: one shared core (`sensor_core/`) wrapped as `SessionScope.vst3`, `SessionScope.clap` and, on macOS, `SessionScope.component` (Audio Unit). A pass-through effect for the master track that publishes one record per audio block into shared memory |
| `engine/`       | C++: the trace reader, statistics, reports and checks (`src/`), the console program `sessionscope` (`tools/`), the HUD window (`hud/`, GLFW + Dear ImGui, drawn with Direct3D 11 on Windows and OpenGL on macOS) and the tests                                 |
| `hosts/reaper/` | the optional REAPER add-on: JSFX (Sensor, Workload, Fixed Latency), ReaScripts (Record trace, Inventory, Build fixtures), the REAPER extension for realtime CPU, and their tests                                                                               |
| `tools/`        | build, install and check scripts (Python, only for building): `setup.py`, `build_native.py`, `build_engine.py`, `install_plugins.py`, and the validators' runners                                                                                              |
| `docs/`         | decisions, research records per spike, metric definitions                                                                                                                                                                                                      |
| `handoff/`      | for the next AI agent: the HUD's direction, what's next, how to work here                                                                                                                                                                                      |

## Quick start: sensor + HUD window

Needs Python 3.10+ and git, for the build only. On macOS also the Xcode command line tools
(`xcode-select --install`). From the repository folder:

```sh
python tools/setup.py   # once, and after pulling changes: builds and installs everything
```

Then open the HUD: `build/engine/SessionScopeHUD.exe` on Windows, `build/engine/SessionScopeHUD`
on macOS.

In your DAW: put **SessionScope Sensor** (TRIVUNE) on the **master track** and press play. If the
DAW doesn't list it, rescan its plug-ins. Until the sensor plays, the HUD shows what's missing.
The HUD is one long strip of modules, as in MiniMeters:

| Module          | Shows                                                                                                                                                                                                                                               |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Peak meter      | the DAW process's CPU, with a peak mark held for two seconds                                                                                                                                                                                        |
| CPU number      | the DAW's share of the whole machine, averaged over the last second                                                                                                                                                                                 |
| Core heatmap    | every core of the machine, numbered on the left (dark blue idle, through cyan, green and yellow, to red full), the newest second at the right; bar starts are lines with the bar number above, like a DAW's timeline; hover for a core's exact load |
| Busiest core    | the single most loaded core and which one it is: a pinned audio thread crackles long before the overall CPU looks high                                                                                                                              |
| Dropouts        | blocks that ran late enough to drop out (count, and the bar of the last one), and the headroom left                                                                                                                                                 |
| Memory and disk | the DAW's RAM, the machine's free RAM, and how fast the DAW reads (sample streaming)                                                                                                                                                                |
| CPU clock       | the CPU's speed against its rated speed, and whether the system caps it (heat, battery, power plan)                                                                                                                                                 |

Using it:

- **Move** a module: drag it. **Resize**: drag the gap between two modules; double-click the gap
  to undo.
- **Right-click** anywhere for the menu: which modules to show, **Pop out** the module under the
  mouse into its own window (close that window to put it back), horizontal or vertical, **Frame
  rate** (the screen's rate, 60 or 30 fps: lower uses less CPU), **Opacity**, keep on top, show in
  taskbar, tray icon, reset layout, quit.
- **Ctrl+Alt+H** shows or hides the HUD from any program (change it with `hotkey=` in `hud.ini`).
- **Tray icon** (optional): click to show or hide, right-click to quit. Turning the taskbar button
  off turns the tray icon on, so the HUD can't get lost.

The DAW's readings need the sensor playing (it tells the HUD which process is the DAW); the core,
memory and clock modules work without it. Tray icon, hotkey and taskbar choice are Windows-only.

`--demo` shows the HUD on generated data, without a DAW (its CPU number is a stand-in); `--replay
<trace folder>` replays a recording.

The HUD remembers its place, size and these choices in `hud.ini` in your SessionScope settings
folder (`%APPDATA%\SessionScope` on Windows, `~/Library/Application Support/SessionScope` on
macOS); delete the file to start fresh.

|                             | Windows                                           | macOS                                                 |
| --------------------------- | ------------------------------------------------- | ----------------------------------------------------- |
| Formats                     | VST3, CLAP                                        | VST3, CLAP, Audio Unit                                |
| Per-user plug-in folders    | `%LOCALAPPDATA%\Programs\Common\VST3`, `...\CLAP` | `~/Library/Audio/Plug-Ins/VST3`, `CLAP`, `Components` |
| Shared folders (need admin) | `%CommonProgramFiles%\VST3`, `...\CLAP`           | `/Library/Audio/Plug-Ins/...`                         |

`setup.py` installs into the per-user folders and tries the shared ones. Some DAWs only scan the
shared folders; then run `python tools/install_plugins.py` from an administrator terminal (or with
`sudo` on macOS). Pro Tools (AAX) is not supported: it needs an Avid developer account.

## Record and analyse

The console program is `build/engine/sessionscope` (`.exe` on Windows):

```sh
sessionscope record                     # record from the sensor, Ctrl+C stops
sessionscope list                       # recorded traces, oldest first
sessionscope report                     # newest trace: passes, bar heatmap, S1/S2 checks
sessionscope report --metric workload   # the known load in fixture F7
sessionscope simulate OUT               # a synthetic trace, labelled as such
```

## REAPER add-on (optional)

Requirements: REAPER 7 (built against 7.77), Node 18+ for its Lua tests. Windows for the extension.

```sh
python hosts/reaper/tools/reaper_install.py      # links the JSFX and scripts into your REAPER
python hosts/reaper/tools/reaper_validate.py     # a real REAPER, isolated settings, dummy audio device
```

On Windows, `setup.py` also installs the REAPER extension when REAPER is present. Traces then
include `host.csv`, and reports add a "Realtime CPU" heatmap. In REAPER: _Actions > Show action
list > New action > Load ReaScript_, and load the three `SessionScope - ...` scripts from
`Scripts/SessionScope`.

1. Run **SessionScope - Build fixtures**. It writes the test projects to
   `<REAPER resource path>/SessionScope/fixtures/` without touching your open projects.
2. Open a project (a fixture or your own), put **SessionScope Sensor** on the master track.
3. Run **SessionScope - Record trace**, play, and run it again to stop.
4. `sessionscope report` reports on it.

## Tests

Building and testing use CMake (installed portable in `C:\tools`; see `handoff/cmake-move.md`):

```sh
cmake --preset windows                                # once: set up build/cmake ("mac" on a Mac)
cmake --build --preset windows                        # build everything that changed
ctest --preset windows                                # engine, HUD and sensor-core tests (C++)
ctest --preset windows-plugins                        # Steinberg's VST3 validator, clap-validator, a CLAP host
                                                      # (macOS adds auval; close your DAW first)
cd hosts/reaper/tests && npm install && npm test      # REAPER add-on: Lua modules under a fake REAPER
```

There is no CI set up at the moment. The Python scripts in `tools/` still install the plug-ins and
run the REAPER tools until the CMake move's last phase replaces them.

## Where this is going

[STATUS.md](STATUS.md) lists what is done and what is next. In short: finish the P1 spikes,
decide at gate G1 between path A (per-track timing) and path B (bar heatmap, latency, paired
before/after comparisons), then build the pipeline (P2), diagnostics (P3) and the HUD.
