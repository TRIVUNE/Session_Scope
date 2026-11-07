# SessionScope

Performance diagnostics for digital audio workstations, by TRIVUNE: find where in the song a
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

| Path | What it is |
| --- | --- |
| `native/` | the sensor: one shared core (`sensor_core/`) wrapped as `SessionScope.vst3`, `SessionScope.clap` and, on macOS, `SessionScope.component` (Audio Unit). A pass-through effect for the master track that publishes one record per audio block into shared memory |
| `engine/` | C++: the trace reader, statistics, reports and checks (`src/`), the console program `sessionscope` (`tools/`), the HUD window (`hud/`, GLFW + OpenGL + Dear ImGui) and the tests |
| `hosts/reaper/` | the optional REAPER add-on: JSFX (Sensor, Workload, Fixed Latency), ReaScripts (Record trace, Inventory, Build fixtures), the REAPER extension for realtime CPU, and their tests |
| `tools/` | build, install and check scripts (Python, only for building): `setup.py`, `build_native.py`, `build_engine.py`, `install_plugins.py`, and the validators' runners |
| `docs/` | decisions, research records per spike, metric definitions |
| `design/hud-prototype/` | the HUD interaction prototype (illustrative data only) |

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
Once it does, the HUD shows a lateness spectrogram and its profile, the bar map per pass, the
waveform of late callbacks, the late p99 readout, and a list of bars below.

`--demo` shows the HUD on generated data, without a DAW; `--replay <trace folder>` replays a
recording.

In the HUD, the words next to the level meter are buttons. **pin** keeps it on top, **list** folds
the bar list away, and **compact** shrinks it to a strip with just the waveform and the readout
(**full** brings everything back). Click a column title in the list to sort by it. Click a bar to
see its blocks pass by pass: the median, the p99, the worst single block, and how many overran.
The HUD remembers its place, size and these choices in `hud.ini` in your SessionScope settings
folder (`%APPDATA%\SessionScope` on Windows, `~/Library/Application Support/SessionScope` on
macOS); delete the file to start fresh.

| | Windows | macOS |
| --- | --- | --- |
| Formats | VST3, CLAP | VST3, CLAP, Audio Unit |
| Per-user plug-in folders | `%LOCALAPPDATA%\Programs\Common\VST3`, `...\CLAP` | `~/Library/Audio/Plug-Ins/VST3`, `CLAP`, `Components` |
| Shared folders (need admin) | `%CommonProgramFiles%\VST3`, `...\CLAP` | `/Library/Audio/Plug-Ins/...` |

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
include `host.csv`, and reports add a "Realtime CPU" heatmap. In REAPER: *Actions > Show action
list > New action > Load ReaScript*, and load the three `SessionScope - ...` scripts from
`Scripts/SessionScope`.

1. Run **SessionScope - Build fixtures**. It writes the test projects to
   `<REAPER resource path>/SessionScope/fixtures/` without touching your open projects.
2. Open a project (a fixture or your own), put **SessionScope Sensor** on the master track.
3. Run **SessionScope - Record trace**, play, and run it again to stop.
4. `sessionscope report` reports on it.

## Tests

```sh
python tools/build_engine.py --test                   # engine tests (C++)
python tools/run_vst3_validator.py                    # Windows: Steinberg's VST3 validator
python tools/run_clap_validator.py                    # clap-validator, and a host that reads the records
python tools/check_mac_plugins.py                     # macOS: VST3 validator, clap-validator, auval
cd hosts/reaper/tests && npm install && npm test      # REAPER add-on: Lua modules under a fake REAPER
```

CI (`.github/workflows/ci.yml`) runs these on Windows and macOS for every push and every pull request.

## Where this is going

[STATUS.md](STATUS.md) lists what is done and what is next. In short: finish the P1 spikes,
decide at gate G1 between path A (per-track timing) and path B (bar heatmap, latency, paired
before/after comparisons), then build the pipeline (P2), diagnostics (P3) and the HUD.
