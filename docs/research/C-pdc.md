# Spike C: declared plugin delay (PDC)

- Date: 2026-10-27 · Host: REAPER 7.77 x64 · Status: **passed for JSFX**; third-party plugins open

## Question

Does `TrackFX_GetNamedConfigParm(track, fx, "pdc")` return each FX's declared delay in samples,
in every kind of chain slot, and what does it say for bypassed and offline FX (blueprint 10.4 C,
test T02)?

## Setup

Fixture F4 with the **SessionScope Fixed Latency** JSFX, which delays audio by exactly its
slider's number of samples and declares that number to REAPER (`pdc_delay`). Instances in a
track chain, a track's input FX, inside a REAPER 7 container, on another track and on the master.
Played for 2.5 s on the dummy device at 48 kHz so every JSFX ran, then read. Then the bypassed
instance (512) was enabled, played 1.5 s, bypassed again, and read straight away and 1.5 s later.

## Observed (validation runs 1 and 2, identical)

| Slot | Slider | Reported |
| --- | ---: | ---: |
| Track chain, active | 1024 | 1024 |
| Track input FX | 64 | 64 |
| Inside a container | 256 | 256 |
| Another track | 128 | 128 |
| Master chain | 256 | 256 |
| Bypassed | 512 | 0 |
| Same instance enabled and running | 512 | 512 |
| ... right after bypassing it | 512 | 0 |
| Offline | – | 0 (slider reads 0 too: the plugin is unloaded) |

Unit conversion: 1024 samples at 48 kHz = 21.333 ms (`pdc_ms` in the inventory).

## Result

For JSFX, `pdc` is exact in every slot tested. A bypassed FX reports 0 at once: REAPER does not
count the delay of bypassed FX, so the chain model (serial sum of enabled FX) agrees with REAPER
here. Offline FX report 0.

## Limitations and next

- Only JSFX tested. Repeat with third-party VST3 and CLAP plugins of known latency (a linear-phase
  EQ, a look-ahead limiter) and compare with REAPER's own PDC display in the FX window.
- A JSFX declares its delay only once it has run: read after playback has started.
- The container's own `pdc` value was not compared with its children's sum; to do.
- The modelled chain sum is not REAPER's compensated latency, and is labelled as a model.
