# SessionScope: status and next steps

Last updated 11 November 2026. The working plan and next steps in one place: [guide.md](guide.md). Phases follow the blueprint's build plan (P0 → P7, gate G1).
Where things are in the repository: see the table in [README.md](README.md).

## Done

### P0: foundations
- [x] Repository layout, `.gitignore`, `.editorconfig`, CI workflow (`.github/workflows/ci.yml`: Lua syntax check and tests; engine build and tests; VST3 and CLAP validators on Windows; the macOS build with all three validators)
- [x] Decision log and records (`docs/decisions/`), research record per spike (`docs/research/`), metric dictionary (`docs/metrics.md`)
- [x] Test fixtures F1–F5, F7a, F7b built by script through REAPER's API (`hosts/reaper/Scripts/SessionScope/lib/fixtures.lua`), F6 (broken streams) in the simulator
- [x] Synthetic workload JSFX with a known, steppable cost (F7) and a fixed-latency JSFX (F4)

### P1: REAPER proof of concept
- [x] **Sensor v0** as a JSFX on the master track: one record per audio block into shared memory (`SessionScope Sensor.jsfx`)
- [x] **Recorder** script: sensor ring, transport, REAPER's xruns and the known workload into a trace folder (`SessionScope - Record trace.lua`)
- [x] **Inventory** script: tracks, FX, GUIDs, containers, input FX, declared delay (`SessionScope - Inventory.lua`)
- [x] **VST3 sensor** (`native/vst3_sensor`, built with the MIT VST3 SDK) + **HUD window** (`build/engine/SessionScopeHUD.exe`) fed through shared memory. Verified in real REAPER 7.77: 2,599 blocks, none lost, sensor cost 0.3 us median
- [x] **Analyser** (`engine/`, C++; `sessionscope report`): passes, per-bar p99 heatmap, observations, S1/S2 checks, JSON output
- [x] **REAPER extension** measuring REAPER's realtime CPU per block (`hosts/reaper/extension/`), built with a pinned compiler (`tools/get_zig.py`, `tools/build_native.py`)
- [x] **Headless validation** in a real REAPER with an isolated settings folder and the silent dummy audio device (`hosts/reaper/tools/reaper_validate.py`); your own REAPER setup is not touched
- [x] Tests: 49 Lua tests against a fake REAPER and 15 C++ engine test programs, all passing

### Results so far (REAPER 7.77, dummy audio 48 kHz / 256)
| Spike | Result |
| --- | --- |
| A, inventory | Passed: 7 fixtures, containers, input FX, duplicate names all correct |
| B, transport | Passed: starts, stops, seeks, loop wraps and tempo changes tracked, no false continuity |
| C, plugin delay | Passed for JSFX: exact in every slot; bypassed FX report 0 |
| D, CPU per FX/track | Not available from REAPER. **Total realtime CPU is**: the extension tracks a known load within 1.5 points |
| E, overhead | Sensor: p99 ≈ 2 µs per block (0.04 % of the deadline) |
| S1, bar mapping | Passed: 100 % agreement across tempo changes, seeks and loops |
| S2, lateness proxy | Can't be judged on the dummy device; needs a real interface |
| T10, two sensors | Passed: warned, one writer recorded, no false alarm when switching projects |

Two findings that shape gate G1:
1. With anticipative FX **off**, realtime CPU from the extension is an accurate load measurement.
2. With anticipative FX **on** (REAPER's default), the audio thread looks idle until the render-ahead runs dry. Realtime CPU then gives no early warning.

## To do

### Now: finish P1 (needs you at the computer)
- [ ] **S2 on your audio interface.** Install into your REAPER (`build/engine/sessionscope reaper install`; `sessionscope install` puts `reaper_sessionscope.dll` into `%APPDATA%\REAPER\UserPlugins\`), restart REAPER. Open `SessionScope/fixtures/F7b/F7b.rpp` with the Komplete Audio ASIO driver at 48 kHz / 256, run *SessionScope - Record trace*, play bars 1–24 twice, stop. Repeat with F7a, then both at 128 samples. Then `build/engine/sessionscope.exe report` for each. Steps in `docs/research/S2-lateness.md`.
- [ ] **Spike E.** On fixture F2, note REAPER's Performance Meter (RT CPU, longest block) with nothing loaded, then with the sensor, then with sensor + recorder + extension.
- [ ] **Spike C with real plugins.** Add a third-party linear-phase EQ and a look-ahead limiter, run *SessionScope - Inventory*, compare the reported delay with REAPER's own PDC display in the FX window.

### Next: finish P1 (can be done without you)
- [ ] Look for a documented way to read REAPER's anticipative render-ahead margin (the early warning for finding 2).
- [x] Fixture F8 with time-signature changes: REAPER's bars right; the JSFX's quarter notes drift inside 7/8 (S1 note).
- [x] FX identity across rename, reorder, copy, move and save + reopen: passed 10/10 (spike A note).
- [x] VST3 sensor records the host's bar start (`barPositionMusic`, layout 2); the HUD counts bars from it.
- [x] VST3: Steinberg's `validator` passes (47/47); installed as a proper `.vst3` bundle.
- [x] CLAP sensor (`native/clap_sensor`): `clap-validator` 18 passed, 0 failed (26 skipped: features the sensor has none of), and a test host reads its records back (`python tools/run_clap_validator.py`).
- [ ] Test the VST3 and CLAP sensors in other hosts (Bitwig, Cubase, Live); then S3 (probe pairs) if still wanted.
- [x] HUD rebuilt as a web UI (Electron window) with realtime CPU from the extension's shared memory and a CPU-per-bar chart.
- [x] Engine and HUD ported to C++ (`engine/`): one native process instead of Python + Electron; the Python analyser and the web HUD are removed.
- [x] HUD redesigned as a MiniMeters-style strip of modules (Nov 2026): peak meter, DAW CPU, core heatmap with bar lines, busiest core, dropouts, memory and disk, CPU clock. Drag to reorder, drag gaps to resize, pop modules out into their own windows, horizontal or vertical, frame-rate limit, opacity, tray icon, hide from taskbar, Ctrl+Alt+H, real Windows menus. Direct3D 11 + DirectComposition on the screen's own GPU. Code map: `engine/hud/README.md`.
- [x] Task Manager, phase 1: the probes' shared table (`native/sensor_core/probe_table.h`), the reader (`engine/src/probe_monitor.*`), the module (CPU and peak per track, click a track for its plug-ins), simulated probes in `--demo`. Plan: `handoff/task-manager-plan.md`.
- [x] **Task Manager, phase 2: the SessionScope Probe VST3 plug-in** (`native/vst3_probe`, Windows) loads a real plug-in inside it and times it (audio, parameters, editor, state, latency passed through; a picker for the plug-in). Steinberg's validator passes (47/47). **Checked in a real REAPER 7.83 (11 Nov 2026):** seven third-party VST3 effects (Crisp, KClip3, Fuse Compressor, Flex Chorus, Duck, kHs Chorus, Crossover) were put in Probes in a running project; parameters, values, latency and bypass were the same as without the Probe; each was timed live and again after save + reopen; taking them out again gave back the original plug-ins unchanged. Details: `handoff/task-manager-plan.md`.
- [x] Task Manager, phase 3: track names and positions from the host (VST3 `IInfoListener`; REAPER sends them); the CLAP probe (`native/clap_probe`: a timed "twin" of every installed CLAP plug-in; clap-validator and a test host pass); wrapping every VST3 effect on a track in REAPER (*SessionScope - Probe all FX* / *Remove probes*, `lib/probes.lua`); `sessionscope probes` prints the Task Manager's numbers in a terminal.
- [x] Probes in REAPER 7.83, by script (11 Nov 2026): the picker window (a plug-in picked by sending its controls the messages of a click; the plug-in's own editor shows after reopening) and the CLAP probe (the twin of a third-party CLAP plug-in loaded with all its parameters and was timed).
- [ ] Probes, still to check by hand: the VST3 Probe in Cubase and Ableton Live; the CLAP probe in Bitwig; a look at the windows by eye. Set `SESSIONSCOPE_PROBE_LOG` to a file path before starting the DAW to get a log of what the DAW asks the Probe.
- [x] HUD: scrolling in the Task Manager when there are more rows than fit (mouse wheel, a thin scroll bar).
- [ ] HUD: macOS versions of the tray icon, hotkey and menus (Windows-only now).
- [x] Old HTML prototype (`design/hud-prototype/`) deleted: `SessionScopeHUD.exe --demo` shows the design.
- [ ] HUD: a settings panel (hotkey), an installer (see guide.md).
- [x] **Build with CMake instead of Python** (decided Nov 2026, done 11 Nov 2026). Every script in `tools/` and `hosts/reaper/tools/` moved to CMake or into the console program (fetching toolkits with checked hashes, the Zig compiler, engine + HUD + tests, sensors, validators, installing plug-ins, the REAPER install/validation); the Python scripts are deleted and the docs updated.
  - [x] Phase 1: engine, console program, HUD and tests build with CMake (`cmake --preset windows`, `cmake --build --preset windows`, `ctest --preset windows`); GLFW, Dear ImGui, stb, Inter and Zig are fetched and checked by CMake. 18/18 tests pass.
  - [x] Phase 2: the sensors and the REAPER extension build with CMake (`native/CMakeLists.txt`); `ctest --preset windows-plugins` runs Steinberg's VST3 validator, clap-validator and the CLAP test host (3/3 pass). macOS bundles, Audio Unit and auval are written (`native/mac.cmake`) but not yet run on a Mac.
  - [x] Phase 3 (11 Nov 2026): `sessionscope install` and `sessionscope reaper install|uninstall|validate` replace the Python scripts (`engine/src/install.*`, `engine/src/reaper_tools.*`, with tests); `cmake --workflow --preset windows` sets up, builds, tests and installs in one command; every Python script is deleted; README, guide and handoff updated. What replaced what: `handoff/cmake-move.md`.
- [ ] **The big check, in 4 passes** (after the CMake move):
  1. Build: a fresh clone builds with CMake alone, every test passes, no warnings.
  2. Bugs: a correctness review of the engine, the HUD and the plug-ins.
  3. Readability: every file says what it does and where it comes from; README, guide, handoff and this list match the code.
  4. Live: end to end with a real DAW (sensor, HUD, probes), CPU and memory measured.

### Gate G1: choose the MVP path
- [ ] Write the decision record: path B (bar heatmap + latency + before/after comparisons) looked likely while per-FX/per-track CPU wasn't available; the Probe (Task Manager phase 2) would make it available in any VST3 host, so revisit once it works. Decide which metric the heatmap shows in REAPER: realtime CPU (extension), the sensor proxy (after S2), or both.

### P2 onward (after G1)
- [ ] P2: versioned measurement schema, local connection between sensor/extension and the HUD, storage, reconnect, golden-trace replay tests.
- [ ] P3: statistics engine, the heatmap UI, recommendation rules, before/after comparison workflow.
- [ ] P4: Windows installer (sensor + extension + scripts + HUD), private alpha, then closed beta.
- [ ] P5: other DAWs. The VST3, CLAP and Audio Unit sensors exist and pass their validators (decision 0005); not yet tried in hosts other than REAPER. P6: paid launch.
- [ ] AAX (Pro Tools): blocked, needs an Avid developer account.

## Housekeeping
- REAPER here is 7.83 since validation run 4 (30 Oct), the version the blueprint pins; runs 1-3 were on 7.77.
- Build output (`build/`) and the compiler (`.toolchain/`) are not in git; `cmake --workflow --preset windows` recreates them.
