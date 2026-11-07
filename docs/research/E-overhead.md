# Spike E: what SessionScope costs the audio thread

- Date: 2026-10-27 · Host: REAPER 7.77 x64 · Status: **sensor measured**; full comparison open

## Question

Does monitoring harm the deadlines it watches (risk R02)? Target from blueprint 16.1: at most 1 %
more host-reported realtime time on a median workload, and a few µs per block for the sensor.

## Observed

The sensor JSFX times its own `@block` with `time_precise()` and reports it with the next record
(`self_us`). On the dummy device, 48 kHz / 256 samples, over about 25,000 blocks across fixtures
F7a, F7b and F5 (validation run 1):

| | median | p99 | max |
| --- | ---: | ---: | ---: |
| Sensor cost per block | 0.5–0.6 µs | 1.6–2.8 µs | 39–264 µs |

The p99 is about 0.05 % of the 5.33 ms deadline. The maximum is one rare outlier per trace,
likely the thread being preempted between the two clock reads, not work in the sensor.

The recorder runs on REAPER's main thread (defer, about 30 Hz) and never touches the audio thread.
It drains up to 50,000 records per tick; no records were lost in any validation trace.

## Not yet measured

- REAPER's Performance Meter (RT CPU, longest block) with and without the sensor, recorder and
  extension on fixture F2: the comparison the blueprint asks for.
- The extension's hook cost (two clock reads and a 32-byte store per block, by design).
- A real interface rather than the dummy device.
