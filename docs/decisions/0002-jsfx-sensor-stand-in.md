# 0002: A JSFX sensor stands in for the native sensor in P1

- Date: 2026-10-27
- Status: accepted

## Problem

The blueprint's P1 needs a sensor on the master track to run spikes S1 (bar mapping) and S2
(lateness proxy). The native VST3/CLAP sensor needs a C++ toolchain, SDKs and a build pipeline,
none of which exist yet. Waiting for them would delay the measurement questions that decide gate G1.

## Evidence

REAPER's JSFX documentation (reaper.fm/sdk/js, checked 27 Oct 2026) gives a JSFX in `@block`:
`samplesblock`, `srate`, `play_state`, `play_position`, `beat_position` (quarter notes), `tempo`,
`ts_num`, `ts_denom`, the high-resolution clock `time_precise()`, and a named shared memory
segment (`options:gmem=name`, about one million slots) that ReaScript reads with `gmem_attach`
and `gmem_read`. That is the native sensor's record (blueprint appendix B), minus the bar start.

## Alternatives

- Build the native sensor first. Right in the end, slower now, and S1/S2 results from it would
  apply to the JSFX equally: both run as FX on the master track under the same REAPER scheduling.
- Poll REAPER from a script only. No per-block timing at all, so S2 could not be run.

## Decision

Sensor v0 is `SessionScope Sensor.jsfx`. It writes the same record layout into a ring in gmem
that the native sensor will write into shared memory, and the recorder script drains it.

## Consequences

- S1 and S2 can run in REAPER now. Their results carry over to a native sensor on the master
  track; overhead (spike E) does not and must be measured again natively.
- A JSFX knows quarter notes and the current signature, not the bar start, so its own bar
  mapping breaks after a time-signature change. The recorder adds REAPER's bar for each block;
  the native sensor must use the host's bar start (VST3 `barPositionMusic`, CLAP `bar_start`).
- REAPER only. Other hosts wait for the native sensor (D06).

## Review when

The native sensor exists, or S2 shows the master-track placement is unusable in REAPER.
