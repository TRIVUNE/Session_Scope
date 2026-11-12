# The plug-in gauntlet (milestone 0.5): do real plug-ins run in our offline host?

- Date: 2026-11-12 · Machine: the person's Windows 11 PC · Status: **first pass, default
  settings only.** Not yet compared with live numbers (that is the calibration step).
- Decision: [`../decisions/0006-offline-session-analyzer.md`](../decisions/0006-offline-session-analyzer.md), point 8.

## Question

Decision 0006 rests on one bet: installed plug-ins, including copy-protected ones, will load and
really process outside their DAW. Scouting predicted 7 to 9 of 10, with the protected ones
either refusing to load or loading and quietly doing nothing. Which is it on this machine?

## Setup

- `sessionscope bench` (new): every plug-in runs in its **own process**
  (`build/native/sessionscope_bench_host.exe`, source `native/bench/bench_host.cpp`), under a
  watchdog (3 minutes, then the process and anything it started are killed). Windows' crash
  window is switched off, so a crash ends the process at once.
- 48 kHz, 256-sample blocks (5.33 ms), one second of warm-up, then 5 s timed. Effects get noise
  plus a click every half second; instruments get a four-note chord on every beat at 120 BPM.
  The plug-in is told it runs in real time, on a high-priority thread with denormals flushed
  to zero, the same as on a DAW's audio thread.
- **Engagement check** on every run: the output must differ from the input (after allowing for
  the plug-in's latency). If it does not, every automatable parameter (up to 512) is moved
  halfway around its range and the plug-in is played for 2 s more. An instrument must not be
  silent.
- Ten plug-ins, the person's pick ("mixed set"): free, licence file, serial number, iZotope
  activation, iLok, UA Connect, Native Access. **All at their default settings**: no presets yet.

## Result

| Plug-in | Copy protection | Kind | Check | Mean | p99 | Mean, µs |
| --- | --- | --- | --- | ---: | ---: | ---: |
| CHOWTapeModel | none (free) | effect | engaged | 5.64 % | 9.27 % | 300.7 |
| Vital | none (free) | instrument | engaged | 2.98 % | 10.08 % | 158.9 |
| ValhallaVintageVerb | licence file | effect | engaged | 0.57 % | 0.84 % | 30.2 |
| Serum 2 | serial | instrument | engaged | 3.37 % | 11.07 % | 179.8 |
| FabFilter Pro-Q 4 | licence key | effect | runs, idle settings | 0.02 % | 0.06 % | 1.2 |
| iZotope Ozone 12 | iZotope activation | effect | runs, idle settings | 0.70 % | 2.12 % | 37.6 |
| oeksound soothe2 | iLok | effect | engaged | 3.18 % | 8.67 % | 169.6 |
| Soundtheory Gullfoss | iLok | effect | engaged | 7.52 % | 12.67 % | 401.0 |
| UADx LA-2A | UA Connect | effect | engaged | 5.11 % | 6.81 % | 272.8 |
| Kontakt 8 | Native Access | instrument | silent | – | – | – |

Shares are the time inside the plug-in divided by the block's length (100 % = this plug-in
alone fills the audio deadline). p99 and max over 5 s are noisy; the means repeated within a few
tenths of a point between runs (Serum 2: 3.61 % and 3.37 %).

## Reading

1. **No plug-in crashed, hung or opened a window.** All ten loaded headless in 0.08 to 8.7 s
   (Kontakt is the slow one).
2. **Every protected plug-in that was meant to process did:** soothe2 and Gullfoss (iLok),
   UADx LA-2A (UA Connect), Valhalla, Serum 2. Ozone 12 is the plug-in the scouting warned
   about ("loads headless, does not engage"). Here it passes sound through unchanged at its
   defaults (no module switched on) and changes it as soon as its parameters move, so it is
   running, not stalled by its protection. The scouting's case may still happen with other
   versions or licence setups; the check stays in every run.
3. **Pass-through is not proof of DRM.** Pro-Q 4 with no bands and Ozone with no modules are
   bit-exact copies of their input. The first version of the check moved only the first 16
   parameters, which did not change either of them. Moving all of them does. Their times are
   for idle settings and say nothing about a real session: the bench labels them
   "runs, idle settings".
4. **Kontakt is silent, as expected:** with no instrument loaded it has nothing to play. It
   needs a saved state with an instrument in it. This cannot be told apart from "blocked"
   without one.
5. Scouting predicted 7 to 9 of 10. On this machine: **9 of 10 run headless, and the 10th
   (Kontakt) is still open.** The bet of decision 0006 holds so far.

## Not measured yet

- **Real settings.** Everything above is at the defaults. Next: a `.vstpreset` per plug-in,
  saved by the person from a DAW (`sessionscope bench X.vst3 --preset file.vstpreset`), above
  all for Kontakt (an instrument loaded), Pro-Q 4 (bands) and Ozone 12 (modules on).
- **Agreement with live numbers** (the calibration gate): the same plug-ins, same settings, in
  REAPER with anticipative FX off, read through the extension's real-time CPU. Until then every
  number here is an *estimate*.
- Block sizes other than 256, and the effect of running blocks back to back (warm caches)
  instead of paced by a sound card.

## How to repeat it

```
build/engine/sessionscope.exe bench "C:/Program Files/Common Files/VST3/CHOWTapeModel.vst3" ^
    "C:/Program Files/Common Files/VST3/Vital.vst3" ... --seconds 5
```

Each plug-in's result (`N-name.json`) and log (`N-name.log`, every step the host took and
anything the plug-in printed) go to `%TEMP%\sessionscope-bench`, or to `--out DIR`.
