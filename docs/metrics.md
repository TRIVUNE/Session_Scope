# Metric dictionary

Every number SessionScope shows has a source, a unit and a window (blueprint 1.4, 5.5).
The words: **time** (a duration), **load** (time as a share of a deadline), **latency** (signal
delay), **headroom** (deadline minus time), **pressure proxy** (an estimate of load from timing).

| Metric | Definition | Unit | Source | Status |
| --- | --- | --- | --- | --- |
| Deadline *D* | frames / sample rate of one block; 256 @ 48 kHz = 5.333 ms | ms | sensor record | documented |
| Callback interval | *t*[k] − *t*[k−1] between consecutive sensor callbacks | ms | sensor `time_precise()` | measured |
| Lateness *λ* | interval − *D*[k−1]: how much later than one block after the previous callback this one began (blueprint 11.4b) | ms, or share of *D* | derived | **proxy, gate S2** |
| Late callbacks p99 | nearest-rank 99th percentile of *λ*/*D* over one bar of one pass | share of a block | derived | proxy |
| Across passes | median of the per-pass values; never a pooled percentile | same | derived | – |
| Injected workload | the Workload JSFX's known CPU time per block / *D* | share of the deadline | fixture sliders | ground truth (fixtures only) |
| Sensor cost | time the sensor spends in its own `@block` | µs per block | sensor | measured |
| REAPER xrun | REAPER's own audio underrun timestamp (`GetUnderrunTime`) | event | host | documented |
| Declared plugin delay | an FX's PDC as REAPER reports it (`TrackFX_GetNamedConfigParm "pdc"`) | samples; ms = 1000 × samples / rate | host | documented, spike C |
| Chain delay (model) | serial sum of enabled FX's declared delay on one chain | samples | derived | **model**, not REAPER's compensation |
| Realtime CPU | REAPER's audio-thread time per block / *D* | share of the deadline | – | not available yet: needs the extension (decision 0003) |

## Rules

- A missing value stays missing. It is never shown or averaged as zero.
- Lateness is never computed across lost records, a sensor restart, a stop or a seek.
- Percentiles need at least 50 blocks in the bar of that pass (`--min-blocks`); fewer shows as "too few", not as a number.
- p99 of fewer than 100 values is the maximum. Say so where it matters.
- Latency and CPU are separate: never ranked, summed or coloured together.
- Thresholds (normal / elevated / at risk / overrun) for the lateness proxy are 10 %, 20 %, 40 % of a block, and for realtime load 70 %, 85 %, 100 % of the deadline, as in the HUD prototype. They are placeholders until S2 and user tests set them.
