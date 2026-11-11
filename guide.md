# SessionScope: working guide

Last updated 6 November 2026. This is the plan in one place: what we are building, where it
stands, and what to do next, in order. [STATUS.md](STATUS.md) keeps the detailed checklist and
results per spike; [README.md](README.md) explains how to build and run things.

## What we are building

A performance diagnostics tool for any DAW that loads VST3, CLAP or Audio Unit plug-ins, with
extra depth in REAPER, where the research started. It answers four questions a
musician asks when a session crackles:

1. **Where in the song does it choke?** A heatmap of load per bar, per pass of the song.
2. **Why?** Which part of the session is responsible (as far as the host lets us measure).
3. **Does a fix help?** Before/after comparisons of the same bars.
4. **Is it getting close?** A live HUD window that stays on top of the DAW.

It is made of these parts, all in this repository:

| Part | What it does | Where |
| --- | --- | --- |
| Sensor (VST3) | The sensor for any host: one record per block (time, transport, tempo, signature, **bar start**, own cost) into shared memory `SessionScope_v2` | `native/vst3_sensor/` |
| Engine | C++: trace reader and recorder, statistics, reports, checks, the HUD's engine, and the console program `sessionscope` (record, list, report, simulate) | `engine/src/`, `engine/tools/` |
| HUD | C++ window (GLFW, Dear ImGui; Direct3D 11 + DirectComposition on Windows, OpenGL on macOS), always on top, drawn every display frame | `engine/hud/` |
| REAPER add-on (optional) | Sensor JSFX, ReaScripts (recorder, inventory, fixture builder), and the extension that times REAPER's own work on every block (realtime CPU) into `host.csv` and shared memory `SessionScope_host_v1` | `hosts/reaper/` |
| Probe (VST3) | Wraps one plug-in and times it, for CPU per track and plug-in (the Task Manager module) | `native/vst3_probe/` |
| Build | CMake: the pinned Zig compiler and toolkits are fetched and checked; `sessionscope install` copies the plug-ins into place | `CMakeLists.txt`, `cmake/` |

Data flow: **sensor / extension → shared memory or trace folder → engine (C++) → HUD window**, or → `sessionscope report`.

### The HUD, in one picture

```
VST3 sensor ──► shared memory "SessionScope_v2" ──────┐
                                                     ├─► HUD engine (10/s) ──► HUD window (every display frame)
REAPER ext. ──► shared memory "SessionScope_host_v1" ┤   engine/src/            engine/hud/
trace folder ──► --replay, or --demo (no DAW) ───────┘
```

One process: the engine does all measuring and statistics; the window only draws. The HUD is a
strip of **modules** (`engine/hud/modules/module.h`), across or down, picked from the right-click menu
and dragged into order: **peak meter** and **CPU number** (the DAW process's CPU), **core
heatmap** (every core, numbered, jet colours, bar lines like a DAW timeline), **busiest core**,
**dropouts**, **memory and disk**, **CPU clock**. Machine readings come from
`engine/src/system_monitor.h`; dropouts from the engine snapshot. The look is a plain light plug-in panel with Inter
(`engine/hud/look/theme.h`). On Windows it draws like MiniMeters: Direct3D 11 into a DirectComposition
flip swap chain with a frame-latency waitable object (`engine/hud/window/renderer_d3d11.cpp`, on the GPU that drives the window's screen). To add a
module, copy `engine/hud/modules/cpu_number.cpp`, change it, and list it in `modules/module_list.*`
(`engine/hud/README.md` has a map of the code); rebuild with
`cmake --build --preset windows`. To try it without a DAW: `build/engine/SessionScopeHUD.exe --demo`,
or `--replay <trace folder>`.

## Where we are

Phase **P1 (proof of concept, researched in REAPER)** is nearly done. The remaining P1 items are below. After P1
comes **gate G1**: choose the MVP path. Then P2 (pipeline), P3 (diagnostics UI), P4 (installer
and alpha), P5 (other DAWs), P6 (paid launch).

Key findings so far (details in STATUS.md and `docs/research/`):
- Per-FX and per-track CPU are **not** available from REAPER. Total realtime CPU is, through the extension.
- With anticipative FX **off**, realtime CPU is an accurate load measure. With it **on** (REAPER's
  default), the audio thread looks idle until render-ahead runs dry, so it gives no early warning.
- The sensor's timeline maps to the right bar (S1 passed in 4/4 with tempo changes, seeks, loops).

## Done 5–6 November: any DAW, all native ([decision 0005](docs/decisions/0005-daw-neutral-native.md))

- **REAPER is an add-on.** Everything REAPER-only moved to `hosts/reaper/`; reports and the HUD
  speak of "the host", not REAPER.
- **One sensor, three formats.** VST3 (Steinberg's validator: 47 passed, 0 failed), CLAP
  (`clap-validator`: 18 passed, 0 failed, 26 skipped for features the sensor lacks) and, on macOS,
  Audio Unit (`auval` in CI). All three are wrappers around `native/sensor_core/sensor_ring.h`.
- **C++ engine and HUD.** The Python analyser and the Electron HUD are gone. Before removal the C++
  versions matched them exactly (reports, JSON, simulator, and the HUD engine frame by frame).
  The native HUD uses about a fifth of the CPU and memory.
- **macOS.** The sensor bundles, the HUD and the console program build on macOS; CI's `macos` job
  builds and validates them, since there is no Mac here.
- **Pro Tools (AAX)**: not built; it needs an Avid developer account.

## Done 30 October

- **F8 fixture**: 16 bars with time-signature changes (4/4, 3/4 at bar 5, 7/8 at bar 9, 4/4 at
  bar 13), looped. Built by `lib/fixtures.lua`, unit-tested, and recorded by the headless validation.
- **FX identity check** (spike A limitation): the validation now edits a copy of F4 (rename FX and
  track, reorder, copy, move to another track, reorder tracks, save, reopen) and checks GUIDs.
- **VST3 sensor bar start**: records the host's `barPositionMusic` (slot 12). Ring layout 2,
  mapping renamed `SessionScope_v2` so old and new builds never misread each other.
- **Bar counting in the live reader** (`shm.BarCounter`): counts bars from the host's bar start,
  so bars stay right after signature changes; remembers counted bars for loops and seeks back.
- **Realtime CPU in the HUD**: the extension publishes every block into `SessionScope_host_v1`
  from its main-thread timer (the audio thread does no extra work); the HUD shows
  "REAPER RT CPU p99 / max" over the last second when the extension is loaded.
- **New HUD UI (web)** replacing the Tk window: header with status pill, bar, tempo, signature
  and buffer; banner for warnings and notes; four readout tiles (late callbacks, REAPER CPU,
  hottest bar of the pass, lost blocks); rolling meter with labelled thresholds and time axis;
  REAPER CPU strip; bar chart per pass + median with the current bar highlighted, hover details,
  a gradient legend, and a **switch between late callbacks and REAPER CPU per bar**. Electron
  shell with a Pin (always-on-top) toggle; `--replay` for UI work; `?snapshot` for screenshots.
- To put REAPER's CPU on the sensor's bars, the extension publishes `time_precise` next to QPC,
  so the reader converts clocks instead of assuming they agree.
- Tests: 49 Lua (fake REAPER), 58 Python, all passing; both DLLs build without warnings.

### Validation run 4 (headless, REAPER **7.83**, dummy 48 kHz / 256, extension 0.2.0)
REAPER here is now 7.83, the version the blueprint pins.

| Check | Result |
| --- | --- |
| Fixtures | all 8 built (F1-F5, F7a, F7b, **F8**) with no problems |
| Spike C (JSFX delay) | unchanged: exact in every slot; bypassed reports 0 |
| **Spike A, identity** | **passed 10/10**: FX GUIDs survive rename, reorder, move to another track, save + reopen; a copy gets a new GUID; track GUIDs survive rename and reorder |
| T10, two sensors | reported, as before |
| F5, F7a, F7b, F8 recordings | 0 dropped records, 0 xruns; extension timed every block |
| S1 on F5/F7a/F7b | 100 % bar agreement, as before |
| **S1 on F8** | REAPER's bars right everywhere. The JSFX's own bar count: 40.8 % (expected after a signature change). **New finding:** the JSFX's `beat_position` counts eighth notes inside 7/8, so its quarter notes drift 3.5 QN per 7/8 bar. Details in `docs/research/S1-bars.md` |

## Next steps, in order

### 1. Needs you at the computer (finishes P1)

Trying the HUD is now two steps (see README): `cmake --workflow --preset windows` once, then
`build/engine/SessionScopeHUD.exe`, and the sensor on the master track. The HUD's checklist says what's missing.

- [ ] **Try another DAW** (Live, Cubase, Bitwig; CLAP in Bitwig or REAPER): the bar and tempo in
      the HUD should follow the DAW's. In Cubase, note whether ASIO-Guard makes the lateness look
      different (it renders ahead, like REAPER's anticipative FX).
- [ ] **Play your own project** with the sensor on the master and watch the spectrogram: on a
      healthy session it is one bright band at 0; plumes rising above +40 % are callbacks that ran
      late. Note where in the song (the bar map) they happen.
- [ ] **Bars after signature changes** (optional): a project with 3/4 or 7/8 bars; the bar number
      should count up with no jumps. (The F8 fixture has this, built by *SessionScope - Build fixtures*.)
- [ ] **S2 on the audio interface** (Komplete Audio, ASIO, 48 kHz / 256 then 128): record F7b and
      F7a twice each, then `build/engine/sessionscope.exe report`. Steps in `docs/research/S2-lateness.md`.
- [ ] **Spike E**: Performance Meter on F2 with nothing / sensor / sensor + recorder + extension.
- [ ] **Spike C with real plugins**: a linear-phase EQ and a look-ahead limiter; compare
      *SessionScope - Inventory* with REAPER's PDC display.

### 2. Can be done without you
- [ ] **Render-ahead margin** (early warning with anticipative FX on): no documented runtime
      readout found. The render-ahead *size* is a preference (default 200 ms). Leads to check:
      `get_config_var_string` / SWS `SNM_GetIntConfigVar` for the size; whether the extension can
      infer the margin from the gap between REAPER's audio hook and the sensor's timestamps
      (the sensor runs ahead of the device by up to the render-ahead size). Ask on the REAPER forum.
- [ ] **Run Steinberg's `validator`** on `SessionScope.vst3` (it is built with the VST3 SDK in
      `.toolchain/vst3sdk`; needs a CMake build of the SDK's `validator` target) and fix anything it reports.
- [ ] **VST3 bundle format**: hosts other than REAPER expect `SessionScope.vst3/Contents/x86_64-win/`.
      Done: `sessionscope install` installs the bundle. Still to do: test in another host.
- [x] **HUD comfort**: remembers window size and position (`hud.ini`), a compact mode (meter
      only), click a bar to see its blocks pass by pass.
- [ ] **HUD next**: a settings panel (hotkey, meter window length). The old HTML prototype was
      deleted (Nov 2026); `SessionScopeHUD.exe --demo` shows the design.
- [ ] **HUD packaging**: the HUD and the console program are single native executables; for P4,
      ship them with the sensor in one installer.
- [ ] **Commenting pass**: the older files still need the line-by-line comments:
      `hosts/reaper/Scripts/SessionScope/lib/*.lua` and the JSFX.
- [x] CLAP sensor (`native/clap_sensor`), checked with `ctest --preset windows-plugins`.
- [ ] Spike S3 (probe pairs) if still wanted.

### HUD: lessons from MiniMeters (looked at its install on 30 Oct; nothing copied)
MiniMeters is a single statically linked C++ executable with its own GPU renderer (Direct3D 11,
Vulkan and OpenGL backends, GLSL shaders cross-compiled), a DirectComposition window
(transparent, borderless), pffft for its FFTs, and bundled .ttf fonts. No UI framework. Why it
looks smooth: it draws every display frame on the GPU. What to take from that, in order:
- [x] Draw every display frame while playing; the spectrogram slides between data updates.
- [x] Native C++ window drawn on the GPU (OpenGL), spectrogram as a texture.
- [ ] **Bundle a font** (an OFL-licensed mono and UI font) instead of system fonts.
- [ ] **Frameless, transparent window**, with our own drag area and a close button.
- [ ] Waveform from per-block data streamed like the spectrogram columns, not a fresh 240-column
      snapshot each frame.

### HUD layout today
Meters on top (MiniMeters style: lateness spectrogram + profile, bar map strip, waveform, late
p99 readout) and the **Task Manager view** underneath: every bar of the song as a row, grouped
"Playing now / Hot bars / All bars", totals over the column names, amber-tinted cells, click a
column to sort, "list" folds it away. Rows are bars, not tracks: REAPER doesn't give per-track
CPU (finding D); per-track rows need path A or another host.

### Tools installed on 30 Oct
- Node.js updated 24.18.0 → 24.22.1 (winget), needed by REA.
- **REA** (`rea-agents` 6.8.0) connected to Claude Code: MCP server `rea` in `~/.claude.json`,
  skill in `~/.claude/skills/reverse-engineer-anything`. No Hopper. Restart Claude Code to load it.

### 3. Gate G1: choose the MVP path
- [ ] Write `docs/decisions/0006-mvp-path.md` (template: `0001-record-template.md`). Path B
      (bar heatmap + latency + before/after comparisons) looks likely, since per-FX/per-track CPU
      isn't available. Decide what the REAPER heatmap shows: realtime CPU (extension), the sensor's
      lateness proxy (after S2), or both. S2's result decides this, so do S2 first.

### 4. After G1
- [ ] **P2**: versioned measurement schema, the live connection (shared memory today) as a real
      protocol with reconnect, storage, golden-trace replay tests.
- [ ] **P3**: statistics engine, heatmap UI, recommendation rules, before/after workflow.
- [ ] **P4**: Windows installer (sensor + extension + scripts + HUD), private alpha, closed beta.
- [ ] **P5**: other DAWs: the VST3, CLAP and Audio Unit sensors exist (decision 0005); what
      remains is testing in each host and deeper host data where a DAW offers it. **P6**: paid launch.

## How to check everything still works

```sh
cmake --build --preset windows                               # everything: engine, HUD, sensors, extension
ctest --preset windows                                       # C++ tests
ctest --preset windows-plugins                               # VST3 + CLAP validators (close your DAW)
cd hosts/reaper/tests && npm test                            # Lua, fake REAPER
build/engine/sessionscope.exe reaper validate                # real REAPER, isolated, ~5 minutes
build/engine/SessionScopeHUD.exe --replay <trace> --screenshot out.png   # one HUD frame to a PNG
```

The validation never touches your own REAPER settings: it uses `build/reaper-validate/` as
REAPER's resource folder and the silent dummy audio device.
