# Spike D: per-track and per-FX CPU time

- Date: 2026-10-27 · Host: REAPER 7.77 (API reference for 7.x) · Status: **negative for per-FX/per-track; total realtime CPU measured** via the extension

## Question

Can an outside tool read REAPER's CPU time per FX or per track, as shown in REAPER's
Performance Meter and FX Bay (blueprint 10.3, risk R01, open question Q01)?

## Method

Searched the complete ReaScript API reference (reaper.fm/sdk/reascript/reascripthelp.html,
downloaded 27 Oct 2026, ~960 KB) for CPU, performance, timing, longest block, xrun, underrun and
dropout. Read the extension SDK headers (`reaper_plugin.h`, `reaper_plugin_functions.h`,
github.com/justinfrankel/reaper-sdk at `b2ca1ec`). Built the extension that decision 0003
proposes and validated it on fixtures F7a and F7b.

## Findings

1. **No ReaScript function returns CPU time** for an FX, a track or the project. The
   `TrackFX_GetNamedConfigParm` keys (pdc, fx_type, fx_ident, container data, `GainReduction_dB`,
   `is_instrument`, ...) include nothing about time. What the Performance Meter shows is not
   exposed to scripts.
2. **`GetUnderrunTime()`** returns the timestamps of REAPER's last audio xrun and media xrun. Not
   CPU time, but REAPER's own record of real dropouts: the recorder logs it as ground truth.
3. **`I_PERFFLAGS`** (per track: no media buffering, no anticipative FX) can be read and set. That
   lets fixtures F7a and F7b compare anticipative processing on and off.
4. **The extension SDK's audio hook** (`audio_hook_register_t::OnAudioBuffer(isPost, ...)`,
   registered with `Audio_RegHardwareHook`) is called on the audio thread before and after REAPER
   processes each device block. The time between the two is REAPER's realtime work for that
   block. Built as `native/reaper_extension` (now `hosts/reaper/extension/`) and measured:

### F7b: anticipative FX off (every track renders on the audio thread)

| Bars | Known Workload cost | REAPER realtime CPU, p99 |
| --- | ---: | ---: |
| 1–2 | 0 % | 0.9 % |
| 3–4 | 9.4 % | 11.1 % |
| 13–14 | 56.2 % | 57.2 % |
| 19–20 | 84.4 % | 85.3 % |
| 23–24 | 103.1 % | 103.8 % |

Every step agrees within about 1.5 percentage points; the small constant excess is REAPER's own
processing. The extension measures realtime load directly.

### F7a: anticipative FX on (REAPER's default)

The audio thread stays at 0.5–0.6 % at every step from 0 % to 94 % known load: the Workload runs
on REAPER's worker threads, ahead of time. In bar 24, after two bars above 100 %, the render-ahead
buffer runs dry and the audio thread's p99 jumps past the deadline (555 % in validation run 4,
104 % in run 5).

## Result

- Per-FX and per-track CPU time: **not available**. This points to path B at gate G1.
- Total realtime CPU: **available and accurate** through the extension, when tracks render in real
  time (anticipative FX off, armed or monitored tracks).
- With anticipative FX on, realtime CPU is near zero until the render-ahead runs out, so it gives
  no early warning. Measuring how full REAPER's render-ahead buffer is would be the early signal;
  no documented API for that is known yet (open question for G1).

## Next

- Spike E with the extension loaded (Performance Meter on F2, with and without).
- Repeat F7a/F7b on a real interface.
- Look for a documented way to read the anticipative render-ahead margin.
