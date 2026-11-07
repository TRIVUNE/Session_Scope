# 0003: Next native pieces: a REAPER audio-hook extension, then the CLAP/VST3 sensor

- Date: 2026-10-27
- Status: accepted for part 1 (the extension is built and validated, see docs/research/D-timing.md); part 2 (native sensor) not started

## Problem

The HUD prototype's main lane is REAPER's realtime CPU: the audio thread's time per block as a
share of the deadline. ReaScript has no call that returns it (the full ReaScript API reference,
v7.x, was searched for CPU, performance and timing functions: none; see spike D). The sensor's
lateness proxy may or may not stand in for it (S2), and in REAPER's default anticipative mode it
probably cannot (blueprint R11).

## Evidence

The REAPER extension SDK (`reaper_plugin.h`, github.com/justinfrankel/reaper-sdk) documents
`audio_hook_register_t`, registered with `Audio_RegHardwareHook`:

```c
void (*OnAudioBuffer)(bool isPost, int len, double srate, struct audio_hook_register_t *reg);
// called twice per frame, isPost being false then true
```

It runs on the audio thread "before and after REAPER's processing" of each device block. The
time between the two calls is the time REAPER spent producing that block: realtime load measured
directly, with documented API only.

## Decision (proposed)

1. A small REAPER extension (`reaper_sessionscope`, C++) registers the hook. In the callback it
   only reads the clock and writes `{t, duration, frames, rate}` into a preallocated lock-free
   ring (blueprint 9.4). A timer on REAPER's main thread drains the ring into `host.csv` in the
   trace folder the recorder announces through ExtState. That gives `metric.host_rt` and lets S2
   compare the sensor's proxy with real realtime load, not only with the injected Workload.
2. Then the native sensor: CLAP first (a header-only, MIT-licensed C API, so the plugin builds
   with any C++ compiler), with VST3 added through the VST3 SDK once the build pipeline exists.
   Same record and ring layout as the JSFX v0 (decision 0002), plus the host's bar start.

## Consequences

- Needs a C++ toolchain (MSVC Build Tools or clang) and CI builds of a Windows DLL.
- The extension is REAPER-only. Other hosts get realtime load only if they have an equivalent, so
  the HUD's realtime lane is REAPER's, and the sensor proxy remains the cross-DAW lane.
- Gate: the extension's own cost must stay within the overhead budget (spike E), and
  disconnecting or crashing the HUD must never touch the audio thread (T04).

## Review when

Spike S2 results are in, or REAPER changes the hook's contract.
