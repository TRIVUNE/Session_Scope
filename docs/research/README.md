# P1 research records

One record per feasibility spike (blueprint 10.4, 10.7): question, setup, sources, observed output,
limitations, result, next decision. Negative results count. A spike passes on evidence, never
because the code compiled once.

Automated evidence comes from `python hosts/reaper/tools/reaper_validate.py`: a real REAPER in an isolated
resource folder, on REAPER's dummy audio device, building the fixtures and recording traces, with
the SessionScope extension loaded when it has been built. The dummy device is silent and needs
no driver, but its timing is not a real interface's: see S2.

## Status, 27 October 2026 (REAPER 7.77, Windows 11, dummy audio 48 kHz / 256)

| Spike | Question | Status | Record |
| --- | --- | --- | --- |
| A | Can a script list tracks, FX, GUIDs, bypass/offline state, containers, input FX? | **passed on fixtures** | [A-inventory.md](A-inventory.md) |
| B | Can the transport be traced without false continuity across stops, seeks, loops, tempo changes? | **passed on fixtures** | [B-transport.md](B-transport.md) |
| C | Does `pdc` return the declared delay, in samples, for every kind of chain slot? | **passed for JSFX** (bypassed FX report 0); third-party plugins open | [C-pdc.md](C-pdc.md) |
| D | Can per-track or per-FX CPU time be read from REAPER? | **no** for per-FX/per-track; **total realtime CPU measured** by the extension (within 1.5 points on F7b) | [D-timing.md](D-timing.md) |
| E | What does SessionScope cost the audio thread? | sensor measured (p99 ≈ 2 µs per block); Performance Meter comparison open | [E-overhead.md](E-overhead.md) |
| S1 | Does the sensor's musical position map to the right bar? | **passed** (4/4, tempo changes, seek, loop) | [S1-bars.md](S1-bars.md) |
| S2 | Does callback lateness track deadline pressure? | **inconclusive: dummy device unusable**, needs a real interface | [S2-lateness.md](S2-lateness.md) |
| S3 | Do probe pairs time a chain? | not started (needs the native sensor) | – |
| T10 | Two sensor instances: warned, one source used? | **passed**: reported after 1 s, one writer recorded, no false alarm on handover | [S1-bars.md](S1-bars.md#t10) |

## What gate G1 still needs

1. S2 on a real interface (the Komplete Audio ASIO here), F7b and F7a, at 256 and 128 samples,
   with the extension loaded so the proxy can be compared with measured realtime CPU.
2. A way to see deadline pressure with anticipative FX on (REAPER's default), where realtime CPU
   stays near zero until the render-ahead runs out (D-timing.md).
3. Spike C with third-party VST3/CLAP plugins of known latency, read against REAPER's own display.
4. Spike E: REAPER's Performance Meter with and without sensor, recorder and extension on F2.
