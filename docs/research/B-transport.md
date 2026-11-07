# Spike B: transport trace

- Date: 2026-10-27 · Host: REAPER 7.77 x64 · Status: **passed on fixtures**

## Question

Can REAPER's transport be polled during play, stop, seek, loop and tempo changes without false
continuity, so that loops become separate passes (blueprint 10.4 B, test T03)?

## Setup

`lib/transport.lua` (event detection from polled samples) inside `lib/recorder.lua`, polling
`GetPlayStateEx`, `GetPlayPosition2Ex`, `GetPlayPositionEx`, loop range, repeat, play rate and
`TimeMap` about 30 times a second. Passes are also derived independently from the sensor's
per-block positions in the analyser (`blocks.py`), so the two can be compared.

## Observed

Fixture F5, recorded by the headless validation (run 2): start at bar 13, play through the
tempo change at bar 17, seek from bar 21 to bar 45 while playing, play through the tempo change
at bar 49, stop at bar 53. `events.csv`:

```
start   at 24.000 s
tempo   120.000 -> 128.000 bpm
seek    39.504 -> 84.500 s
tempo   128.000 -> 100.000 bpm
stop    at 101.625 s
```

Fixtures F7a and F7b, repeat on over bars 1–24: the wrap from bar 25 to bar 1 is a new pass in
both the recorder and the analyser ("jump_back", bars 1–2 after bars 1–24). The analyser found
the F5 seek as a separate pass ("jump", bars 45–53) from the sensor alone.

## Limitations

- Polling at 30 Hz places events to within about 33 ms; per-block truth comes from the sensor.
- Record start/stop and play-rate changes are covered by unit tests only.
- Time-signature changes are detected (`timesig` event) in tests, not yet in a REAPER fixture.

## Result

Passed: no false continuity across seek, loop wrap or stop; tempo changes are events.
