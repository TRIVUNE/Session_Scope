# Plan: the per-track Task Manager

The person wants a Task Manager module: every track, each expandable into its plug-ins, with the CPU
each one uses. A plug-in can't see other plug-ins' CPU on its own, so the person chose a **wrapper
plug-in** ("SessionScope Probe"): it loads the real plug-in inside itself, passes everything
through, and times each audio block it spends inside the real plug-in.

What "CPU" means here: time spent inside the plug-in / real-time length of the block (frames ÷
sample rate). 25 % = a quarter of the time available for the whole block went to this plug-in.
Summing every probe gives the DAW's audio load, the way DAWs report plug-in load.

## Phase 1: the data path and the module (started 2026-11-07)

- `native/sensor_core/probe_table.h`: a shared-memory table ("SessionScope_probes_v1"), header-only,
  used by the probe (writer) and the engine (reader), like `sensor_ring.h`. One slot per probe:
  owner id, heartbeat, blocks, total cost, total budget, peak (taken by the reader), track name,
  plug-in name, track order, process id. Names change rarely and use a sequence counter (seqlock);
  counters are lock-free atomics, so the audio thread never waits.
- `engine/src/probe_monitor.*`: reads the table ten times a second, turns counter differences into
  CPU % per probe, groups probes by track.
- `engine/hud/modules/task_manager.cpp`: the module. Tracks in DAW order (or by CPU when the CPU
  column title is clicked), click a track to show its plug-ins, CPU and peak with a small bar.
- Demo: simulated probes write into a private table through the real writer, so `--demo` shows the
  module working and tests cover the real path.

## Phase 2: the Probe plug-in (VST3)

- `native/vst3_probe/`: a VST3 effect that hosts one VST3 plug-in, using the SDK's hosting helpers
  (`public.sdk/source/vst/hosting`: module loading, host classes, process data).
- Passes through: audio buses and arrangements, parameters (the probe's controller forwards to the
  inner controller), the editor (IPlugView of the inner plug-in), state (probe state = which
  plug-in + its state), latency, tail, MIDI events.
- Picker: the probe's own editor lists installed VST3 plug-ins (standard VST3 folders) until one is
  chosen.
- Times only the inner process() call (QueryPerformanceCounter / mach time), writes to the table.
- Check with Steinberg's validator, then in REAPER and another DAW.

## Phase 3: wider coverage

- CLAP probe (CLAP hosting is a plain C API).
- Track names from the host: VST3 `IInfoListener::setChannelContextInfos`, CLAP `track-info`.
- Investigate a helper that wraps every plug-in on a track (REAPER script first).

## Risks

- Wrapping changes nothing audible but adds a few microseconds per block and one extra editor
  layer; some plug-ins check their host and may misbehave inside another plug-in.
- Plug-ins with sidechains or unusual bus layouts need the probe to mirror their buses.
