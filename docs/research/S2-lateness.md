# Spike S2: does callback lateness track deadline pressure?

- Date: 2026-10-27 · Host: REAPER 7.77 x64 · Status: **inconclusive: needs a real audio interface**

## Question

The sensor sees only its own callbacks. Blueprint 11.4b proposes their lateness,
λ = interval − block duration, as a proxy for deadline pressure. Does it follow load, with
anticipative FX off and on (risk R11)?

## Setup

Fixtures F7b (anticipative FX off on every track and the master) and F7a (REAPER's default, on):
one track with the Workload JSFX in proportional mode, rising by 500 µs per 256 samples every two
bars, from 0 % of the deadline in bars 1–2 to about 103 % in bars 23–24. The recorder knows the
injected cost of every block from the Workload's sliders. 50 s of each, recorded by the headless
validation on REAPER's **dummy audio device**, 48 kHz, 256 samples (5.33 ms).

## Observed (validation run 1)

Late-callback p99 per bar, median of bars per load step, share of a block:

| Known load | F7b (anticipative off) | F7a (anticipative on) |
| ---: | ---: | ---: |
| 0 % | 192 % | 23 % |
| 9–94 % | 24–37 %, no trend | 23–33 %, no trend |
| 103 % | 4 % | 258 % |
| REAPER xruns | 0 | 0 |
| Rank correlation, load vs proxy | −0.40 | +0.19 |

## Reading

The dummy device makes this test meaningless, for two reasons visible in the data:

1. Its callbacks are paced by a timer, not by audio hardware: at zero load, lateness p99 is
   already 23–192 % of a block. Any real signal would drown in it.
2. It never misses a deadline. At 103 % load REAPER reported no xrun, and in F7b the lateness fell
   to about 4 %: the device simply waited for each block, so the callbacks ran back to back,
   each about 103 % of a block apart, slower than real time.

So the lateness proxy cannot be judged on the dummy device. This says nothing about it on real
hardware. The analyser flags dummy-device traces, and S2 reports "inconclusive" for them.

## Next: S2 on real hardware (needs a person at the computer)

1. In your own REAPER (with the scripts installed), open `SessionScope/fixtures/F7b/F7b.rpp`,
   audio device: the interface's ASIO driver, 48 kHz, 256 samples.
2. Run **SessionScope - Record trace**, play bars 1–24 twice, run it again to stop.
3. Repeat with F7a, then both again at 128 samples.
4. `build/engine/sessionscope.exe report <trace>`: the S2 section gives the per-step table, the
   correlation and REAPER's xruns. Expect xruns in bars 23–24 on a real device.

With the extension installed (`build/native/reaper_sessionscope.dll` copied to REAPER's
UserPlugins), the report also compares the proxy with REAPER's measured realtime CPU per bar
("S2 proxy vs realtime CPU"). On the dummy device (validation run 4) that comparison already
confirms the reading above: on F7b the realtime CPU climbs with the load step by step while the
proxy stays flat (rank correlation −0.56), so the proxy's noise there is the device's, not the load.
