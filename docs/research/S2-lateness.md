# Spike S2: does callback lateness track deadline pressure?

- Date: 2026-10-27, real hardware 2026-11-11 · Host: REAPER 7.77 / 7.83 x64 · Status: **answered: no.** On a real interface the lateness of the sensor callbacks does not follow load. It shows a missed deadline only once the audio is already breaking up, and at rest it reads about 50 % of a block because of how the driver spaces its callbacks.

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

## S2 on real hardware (11 Nov 2026)

Setup: Native Instruments Komplete Audio 6 MK2 (USB), "Komplete Audio ASIO Driver", 48 kHz, at
256 and at 128 samples. REAPER 7.83 with the validation's own settings folder (not the person's
REAPER), the extension loaded, started on `hosts/reaper/tests/integration/s2_real.lua`: F7b and
F7a, bars 1-24 twice each (100 s), nothing audible. Four traces, about 18,700 blocks each at 256
and 37,300 at 128, none lost. They are kept in `traces/s2-real-2026-11-11/` (not in git). The
interface was at 44.1 kHz / 512 before and was put back.

### What the driver does

At rest the callbacks do not come every 5.33 ms. They come on a 4 ms grid: **4, 4, 8 ms, repeating**
(at 128 samples: 2, 2, 4 ms). The average is right, but every third callback is 2.67 ms "late":
exactly 50 % of a block. So the proxy reads 50 % with no load at all.

### Late-callback p99 per load step (median of bars, share of a block)

| Known load | F7b, 256 | F7a, 256 | F7b, 128 | F7a, 128 |
| ---: | ---: | ---: | ---: | ---: |
| 0 % | 50.5 % | 50.5 % | 51.2 % | 51.0 % |
| 9-66 % | 50.5-50.8 % | 50.4-50.7 % | 51.2-51.8 % | 50.6-51.1 % |
| 75 % | 49.1 % | 50.6 % | 49.1 % | 50.7 % |
| 84 % | 30.6 % | 50.7 % | 30.8 % | 51.3 % |
| 94 % | 12.0 % | 50.8 % | 11.5 % | 51.1 % |
| 103 % | 3.9 % | 294 % | 4.3 % | 557 % |
| Rank correlation, load vs proxy | -0.58 | +0.34 | -0.60 | +0.20 |
| REAPER xruns | 0 | 0 | 0 | 0 |

### REAPER's realtime CPU from the extension, same traces (median per step)

| Known load | F7b, 256 | F7b, 128 | F7a, 256 and 128 |
| ---: | ---: | ---: | ---: |
| 0 % | 0 % | 0 % | 0 % |
| 9 / 19 / 28 / 38 % | 10 / 19 / 28 / 38 % | 10 / 19 / 29 / 38 % | 0 % |
| 47 / 56 / 66 / 75 % | 47 / 57 / 66 / 75 % | 47 / 57 / 66 / 75 % | 0 % |
| 84 / 94 / 103 % | 85 / 94 / 103 % | 85 / 94 / 103 % | 0 %, p99 454-541 % at 103 % |

### Reading

1. **With anticipative FX off (F7b), the proxy goes the wrong way.** It sits at the driver's 50 %
   until about 75 % load and then *falls*: once a block takes longer than the 4 ms grid, the
   callbacks queue up and come evenly, back to back, so the "late" third disappears. At 103 % the
   audio is breaking up and the proxy reads 4 %: its lowest value.
2. **With anticipative FX on (F7a, REAPER's default), the proxy is flat** at the driver's 50 %
   until the render-ahead runs dry at 103 %, then jumps to several blocks. It reports the
   dropout, it does not warn of it. (The same as on the dummy device.)
3. **REAPER's realtime CPU, measured by the extension, follows the known load within one point**
   at every step with anticipative FX off, at both block sizes. With anticipative FX on it stays
   near zero until the render-ahead runs dry (finding 2 in STATUS.md, now confirmed on hardware).
4. REAPER counted no xruns at 103 % load in any of the four traces, so its xrun counter is not a
   ground truth with this driver either.
5. Bar mapping (S1) held in all four traces: 100 % agreement.

### What follows

- The lateness proxy as defined (interval - block length, late p99 per bar) cannot be the
  REAPER heatmap's metric, and it cannot give an early warning. With anticipative FX off the
  extension's realtime CPU can. With anticipative FX on, nothing measured here warns before the
  dropout.
- **The HUD's Dropouts module is wrong on this interface.** It counts a block as a dropout when
  it is late by 40 % of a block or more, so the driver's every-third-callback pattern counts as
  one: a session using 2 % of the block time showed 749 dropouts, headroom 0.0 %, late p99 51 %
  (`traces/s2-real-2026-11-11/hud-on-asio-256.png`). Any driver that delivers callbacks on a
  grid that the block length does not divide evenly will do this.
- Ways to make it right, for the person to choose from: (a) measure lateness against the driver's
  own pattern (how much later than its usual latest callback), which removes the false count but
  still only reports a dropout when it happens; (b) add "falling behind real time" as the
  dropout sign (callbacks coming slower than the audio they carry, sustained), which is what
  103 % load looks like in F7b; (c) take the count from the host where the host gives one
  (the extension's realtime CPU at or above 100 % in REAPER). (a) and (b) together work in every
  DAW; (c) is exact but REAPER-only.

## How to repeat it

By script, in the validation's isolated REAPER: the steps are at the top of
`hosts/reaper/tests/integration/s2_real.lua`. Or by hand:

1. In your own REAPER (with the scripts installed), open `SessionScope/fixtures/F7b/F7b.rpp`,
   audio device: the interface's ASIO driver, 48 kHz, 256 samples.
2. Run **SessionScope - Record trace**, play bars 1–24 twice, run it again to stop.
3. Repeat with F7a, then both again at 128 samples.
4. `build/engine/sessionscope.exe report <trace>`: the S2 section gives the per-step table, the
   correlation and REAPER's xruns. (REAPER reported none on the Komplete Audio, even at 103 %.)

With the extension installed (`build/native/reaper_sessionscope.dll` copied to REAPER's
UserPlugins), the report also compares the proxy with REAPER's measured realtime CPU per bar
("S2 proxy vs realtime CPU"). On the dummy device (validation run 4) that comparison already
confirms the reading above: on F7b the realtime CPU climbs with the load step by step while the
proxy stays flat (rank correlation −0.56), so the proxy's noise there is the device's, not the load.
