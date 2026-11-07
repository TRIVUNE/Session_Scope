# 0004: Trace format v1

- Date: 2026-10-27
- Status: accepted (research format; the P2 schema, EN07, replaces it)

## Decision

One folder per recording under `<REAPER resource path>/SessionScope/traces/<yyyymmdd-hhmmss>/`:

| File | Rows | Columns |
| --- | --- | --- |
| `sensor.csv` | one per audio block the sensor saw | `seq, instance, t, frames, srate, play_state, play_pos, qn, tempo, ts_num, ts_den, self_us, r_measure, r_beat, r_qn, workload_us` |
| `transport.csv` | REAPER's transport, about 30 per second | `t, pass, play_state, pos, pos2, cursor, rate, repeat, loop_start, loop_end, tempo, ts_num, ts_den, measure, beat, state_count` |
| `events.csv` | starts, stops, seeks, loop wraps, tempo and signature changes, REAPER xruns, lost records, a second sensor | `t, kind, detail` |
| `meta.json` | host, audio device, project regions, recorder version; a summary once stopped | |

- `t` is `time_precise()` in seconds. The sensor and the recorder read the same clock, so rows
  from both files line up. Blueprint 5.5's `monotonic_ts`.
- `seq` numbers every block the sensor wrote. A gap means records were lost, and nothing is
  computed across it. `instance` changes if a different sensor instance takes over.
- `r_measure` (0-based), `r_beat`, `r_qn` are REAPER's position for that block's `play_pos`,
  added by the recorder. `workload_us` is the Workload JSFX's cost for that block, computed from
  its sliders, and empty when there is none.
- Empty cell = unknown. Never read it as zero.

Columns are defined once (`hosts/reaper/Scripts/SessionScope/lib/recorder.lua`, `engine/src/trace.h`)
and a test (`engine/tests/test_recorder_columns.cpp`) fails if the two disagree.
