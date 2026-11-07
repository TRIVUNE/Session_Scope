# Spike S1: does the sensor's position map to the right bar?

- Date: 2026-10-27 · Host: REAPER 7.77 x64, dummy audio 48 kHz / 256 · Status: **passed** (4/4)

## Question

The heatmap places every block in a bar using what a plugin on the master track knows about the
transport. Does that agree with REAPER's own bar for the same moment, across loops, seeks and
tempo changes (blueprint 10.4 S1, test T09)?

## Setup

The sensor JSFX records `play_position` (seconds) and `beat_position` (quarter notes) with the
current tempo and time signature for every block. The recorder adds REAPER's bar for the block's
position (`TimeMap2_timeToBeats`) and polls `GetPlayPosition2` about 30 times a second. The
analyser compares the bar implied by the sensor's quarter notes with REAPER's bar, and the
sensor's position with `GetPlayPosition2` at the same `time_precise()` moment.

## Observed (validation runs 1 and 2)

| Trace | Playing blocks | Bar agreement | Max QN difference | Sensor − GetPlayPosition2, median | p99 \|offset\| |
| --- | ---: | ---: | ---: | ---: | ---: |
| F5 (tempo changes at 17 and 49, a seek) | 6,071 | 100 % | 0.000000 | −2.8 ms | 5.3 ms |
| F7a (anticipative on, loop) | 9,314 | 100 % | 0.000000 | −2.7 ms | 5.3 ms |
| F7b (anticipative off, loop) | 9,377 | 100 % | 0.000000 | −2.7 ms | 5.3 ms |

The sensor's quarter notes equal REAPER's for the same time. The offset is under one block and
has the expected sign: the sensor stamps the start of the block it is processing, while
`GetPlayPosition2` already points at the next one.

## Limitations

- Runs 1-3 were 4/4 only (see the time-signature section below for F8). A JSFX knows quarter notes and the current signature, not where the bar started, so
  after a time-signature change its own bar count goes wrong (unit-tested). The recorder's
  REAPER bar covers REAPER; the native sensor must use the host's bar start.
- On the dummy device. With anticipative FX on, a sensor running ahead of the device would show
  a large positive offset; in F7a it did not. To recheck on a real interface.

## Result

Passed in 4/4 with tempo changes, seeks and loops: the sensor's timeline is a sound basis for the
bar heatmap. Next: a fixture with a time-signature change, and the native sensor's bar start.

## Time-signature changes (validation run 4, 30 Oct 2026, REAPER 7.83)

Fixture F8: 4/4, 3/4 from bar 5, 7/8 from bar 9, 4/4 from bar 13, played from bar 1 through a
loop wrap (6,377 playing blocks, 2 passes, bars 1-16 then 1-3).

| | Result |
| --- | --- |
| REAPER's bar (recorder, `TimeMap2_timeToBeats`) | right in every block: passes 1-16 and 1-3 |
| Bar from the JSFX's quarter notes + current signature | 40.8 % agreement: wrong from bar 6 on, as expected |
| JSFX `beat_position` vs REAPER's quarter notes | equal in 4/4 and 3/4; **in 7/8 it advances in eighth notes**: +3.5 QN per bar in bars 9-12, then a constant 14 QN offset |

So a JSFX's `beat_position` counts the signature's beats, not quarter notes, once the
denominator is not 4. Consequences: the JSFX sensor's `qn` column is only trustworthy in /4
signatures; anything that needs quarter notes from it (the Workload JSFX's bar windows, the
naive bar) must use REAPER's values from the recorder instead. VST3's `projectTimeMusic` is
defined in quarter notes and `barPositionMusic` gives the bar start; the native sensor now
records both (layout 2), and the HUD counts bars from the bar start (`shm.BarCounter`). To
confirm in REAPER with the VST3 on F8 (by hand: the headless validation loads only the JSFX).

## T10

Two sensor instances on one master (validation run 3): the recorder logged `second_sensor` about
one second after start, and the ring held one writer's records only (755 in 4.03 s, 187.5 per
second at 48 kHz / 256). Run 2 had shown a false alarm when a new project's sensor waited to take
over from a just-closed project's sensor; the warning now needs both instances writing for over
a second, and the handover takes 0.25 s instead of 1 s.
