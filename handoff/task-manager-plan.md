# The per-track Task Manager: what was built, and what was learned

The person wants a Task Manager module: every track, each expandable into its plug-ins, with the CPU
each one uses. A plug-in can't see other plug-ins' CPU on its own, so the person chose a **wrapper
plug-in** ("SessionScope Probe"): it loads the real plug-in inside itself, passes everything
through, and times each audio block it spends inside the real plug-in.

What "CPU" means here: time spent inside the plug-in / real-time length of the block (frames ÷
sample rate). 25 % = a quarter of the time available for the whole block went to this plug-in.
Summing every probe gives the DAW's audio load, the way DAWs report plug-in load.

All three phases are done (11 Nov 2026) and checked in REAPER. What is left is trying it in the
other DAWs: see near the end of this note.

## The pieces

| Piece | Where | What |
| --- | --- | --- |
| The shared table | `native/sensor_core/probe_table.h` | Shared memory "SessionScope_probes_v1", one slot per probe: owner, heartbeat, blocks, cost, budget, peak, track name, plug-in name, track position. Counters are lock-free atomics; names use a sequence counter |
| The reader | `engine/src/probe_monitor.*` | Reads the table ten times a second, turns counter differences into CPU % per probe, groups by track |
| The module | `engine/hud/modules/task_manager.cpp` | Tracks in DAW order (or by CPU), click a track for its plug-ins, mouse wheel to scroll |
| In a terminal | `sessionscope probes [--json]` | The same numbers as text (it only looks: the HUD keeps the peaks) |
| VST3 Probe | `native/vst3_probe/` (Windows) | Holds one VST3 effect. `hosted_vst3.*` loads and times it (Steinberg's hosting helpers); `sessionscope_probe.cpp` is the plug-in the DAW sees and the picker window |
| CLAP Probe | `native/clap_probe/` (Windows) | Shows the DAW a timed "twin" of every installed CLAP plug-in: no window, nothing to pick |
| REAPER scripts | `hosts/reaper/Scripts/SessionScope/lib/probes.lua`, *SessionScope - Probe all FX*, *SessionScope - Remove probes* | Swap every VST3 effect on a track for a Probe holding it, and back |
| Demo | `engine/src/demo_probes.*` | Simulated probes through the real writer, so `--demo` shows the module |

## How the VST3 Probe behaves

- Before a plug-in is picked: a stereo pass-through with a picker window. The picker lists the
  `.vst3` files in the standard folders **by file name, without loading them** (loading a few
  hundred plug-ins to fill a list froze the window and risked a crash); only the chosen file is
  opened. A file with several effects in it gets a second list.
- With a plug-in inside: buses, parameters (same ids), window, latency, tail, MIDI mapping and
  state are the hosted plug-in's own. The Probe's saved state = which plug-in + that plug-in's
  two state halves (layout at the top of `sessionscope_probe.cpp`).
- **Track names:** the DAW tells the Probe through VST3's `IInfoListener`
  (`setChannelContextInfos`). REAPER 7.83 sends name and position. A DAW that doesn't leaves the
  plug-in under "Unknown track".
- **Starting an effect mid-session** (`Probe::start_now`): when an effect arrives while the DAW
  already has the Probe running (picked in the window, or handed over as state), the Probe sets
  it up with the layout and sample rate the DAW is already using and switches it on itself.
  Needed because REAPER does not restart a plug-in that asks for it (see below). If the effect
  can't take the DAW's current layout, the Probe passes audio through until the DAW restarts it.
- `SESSIONSCOPE_PROBE_LOG=<file>` (environment variable, set before the DAW starts): one line
  per call the DAW makes to the Probe. This is how the REAPER facts below were found; use it for
  the next DAW.

## What REAPER 7.83 does (measured 11 Nov 2026, isolated REAPER, dummy audio)

- Loading a project: `initialize` → `setActive(true)` (empty Probe) → `setState` → `setActive(false)`
  → `setActive(true)`. So the state arrives while the plug-in is active, and REAPER then restarts it.
- A state set on a live plug-in (script, undo, preset): REAPER calls `setState` and does **not**
  restart the plug-in, even when it asks with `restartComponent(kIoChanged | kLatencyChanged)`.
  It also keeps the input/output count it read when it loaded the plug-in: a hosted effect with a
  sidechain input or extra outputs shows 2 in / 2 out until the project is reopened.
- `TrackFX_GetNamedConfigParm(..., 'fx_ident')` for a VST3: `<path><<number>{<class id, 32 hex>`.
- `'vst_chunk'` for a VST3 (base64): 4 bytes component length, 4 bytes `1`, the component state,
  4 bytes controller length, 4 bytes, the controller state. Not documented; `lib/probes.lua`
  reads and writes it.
- Seven third-party effects (Crisp, KClip3, Fuse Compressor, Flex Chorus, Duck, kHs Chorus,
  Crossover) wrapped live by the script: same parameter count, names and values, same latency
  (KClip3: 31 samples), same bypass; all timed live and after save + reopen; unwrapped to the
  original plug-ins with the same values.
- `restartComponent(kReloadComponent)`: REAPER destroys the plug-in and makes a new one, and the
  new one is not given the old one's state. The automated version of this check (with our own
  Sensor as the wrapped plug-in) is the "probe" step of `sessionscope reaper validate`.

## A bug the live test found

The table handed a slot to a new probe when its owner had been silent for five seconds. A project
with several probes takes longer than that to load, so two probes ended up writing into one slot
(two plug-ins missing from the list, two others with doubled block counts). Now a slot is free
only when it has no owner or its owner's program no longer runs: the owner id carries the process
id in its upper half (`probe_table.h`, `Writer::claim`). `engine/tests/test_probe_monitor.cpp`
covers both cases.

## How the CLAP Probe works, and its limit

A CLAP plug-in is a table of functions plus one pointer to its own memory. The twin is a copy of
the real plug-in's table with `process` (timed), `activate` (to learn the sample rate), `init`,
`destroy` and `get_extension` (only to hear about track renames) replaced. Everything else, and
every extension, is the real plug-in's own, so presets, automation and the window behave as
without the Probe. The limit: a plug-in that compares the table's address with one it kept,
instead of using the pointer inside it, would be confused. The checks twin our own CLAP sensor
(`SESSIONSCOPE_PROBE_CLAP_PATH` narrows where the Probe looks). CLAP gives no track position, so
those tracks sort by name.

## Checked in REAPER 7.83, and still to check by hand

Checked (11 Nov 2026, isolated REAPER, each by a script; nothing was clicked by a person):

- The wrap and unwrap scripts with seven third-party VST3 effects (above).
- **The picker window**: opened by REAPER, 538 plug-ins listed at once, "Crisp" selected and
  "Load effect" pressed by sending the controls the messages a click sends. The effect started
  at once, the Probe showed Crisp's 14 parameters and was timed on its track; after closing and
  reopening the window REAPER showed Crisp's own editor. (This test found that REAPER answers
  `kReloadComponent` by making a fresh Probe without the state: the Probe no longer asks for it.)
- **The CLAP Probe**: REAPER lists the twins ("CLAP: plugdata-fx (SessionScope Probe)"); the twin
  of a third-party plug-in (plugdata-fx) had the original's 516 parameters and was timed, with
  the track's name from CLAP's track-info.

Still to check, because it needs a person in the DAW:

1. **Ableton Live 12** (installed on the person's computer): does the VST3 Probe load, does the
   hosted plug-in's window open, is it timed. Live scans only the shared folder
   (`C:\Program Files\Common Files\VST3`, seen in its `PluginScanner.txt`), so the plug-ins
   must be installed there first: `sessionscope install` from an administrator terminal. On
   11 Nov 2026 that folder still held a Sensor from 30 Oct (0.2.0) and no Probe. Live does not
   tell plug-ins their track (no `IInfoListener`, to our knowledge), so expect "Unknown track".
   **Cubase** was not found on this computer (no program, no settings folder); when there is
   one: the same checks, and whether it sends the track name. Use the log.
2. **Bitwig** or another CLAP host for the CLAP Probe.
3. Looking at it: the picker and a hosted plug-in's window on screen, by eye.
4. macOS: neither Probe is built there yet (the picker is Win32; CLAP bundles need their own
   loading code).

## Risks that remain

- Wrapping changes nothing audible but adds a few microseconds per block and one extra layer;
  some plug-ins check their host and may misbehave inside another plug-in.
- The REAPER scripts don't carry automation envelopes or parameter links over (one undo step
  brings everything back).
