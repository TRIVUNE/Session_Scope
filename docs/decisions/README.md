# Decisions

The blueprint's decision log (edition 1.1, chapter 00) and the architecture decision records
made while building. A record is never edited to say something else later: a new one supersedes it.

## Blueprint decisions, 27 October 2026

| ID | Decision |
| --- | --- |
| D01 | Pivot from performance monitor to diagnostic and triage assistant: interpret, pinpoint, prioritise fixes. |
| D02 | Price €19–€29 one-time, no subscription. |
| D03 | Lead feature: the bar/beat heatmap, load mapped to the song's grid. |
| D04 | MiniMeters model: a small sensor plugin on the master bus, a local connection, a standalone HUD. REAPER adapter kept for deep data. |
| D05 | Phase 1 is a REAPER proof of concept: data access, diagnostics, triage. |
| D06 | Phase 2 rolls out the sensor to other DAWs; per-track probes only if gate S3 passes. |

## Records

| # | Title | Status |
| --- | --- | --- |
| [0001](0001-record-template.md) | Template | – |
| [0002](0002-jsfx-sensor-stand-in.md) | A JSFX sensor stands in for the native sensor in P1 | accepted |
| [0003](0003-native-next.md) | Next native pieces: REAPER audio-hook extension, then the CLAP/VST3 sensor | extension accepted; VST3 and CLAP sensors built |
| [0004](0004-trace-format.md) | Trace format v1: CSV plus meta.json, one folder per recording | accepted |
| [0005](0005-daw-neutral-native.md) | Any DAW, native code: one sensor in every plug-in format, a C++ engine and HUD | accepted |
| [0006](0006-offline-session-analyzer.md) | The offline session analyzer becomes the product | superseded by 0007 |
| [0007](0007-stop.md) | Stop: SessionScope does not stand as a product | accepted |
