# Plug-in state from an Ableton Live set: does the bench get the person's exact settings?

- Date: 2026-11-12 · Ableton Live 12 Suite (the person's own; the set was made by the person) ·
  Status: **passed.** All 13,455 parameter values of ten plug-ins equal, checked through REAPER.
- Follows `rpp-state.md` (the same check for REAPER projects).

## The set

The person made `Desktop\TEST LIVE Project\TEST LIVE.als` in Live 12: the ten gauntlet
plug-ins, one per track, each with their own presets or settings. It was only read, never changed.

## How Live stores a VST3 plug-in

A `.als` is gzip-compressed XML (2.1 MB unpacked here). Per plug-in:

```
<MidiTrack Id="12"> <Name> <EffectiveName Value="1-Vital" /> ...
  <PluginDevice Id="0">
    <On> <Manual Value="true" /> ...                      on/off switch
    <PluginDesc> <Vst3PluginInfo Id="0">
      <Preset> <Vst3Preset Id="2"> ...
        <ProcessorState> hex </ProcessorState>             the component's state (getState)
        <ControllerState> hex </ControllerState>           the controller's, or <ControllerState />
      </Vst3Preset> </Preset>
      <Name Value="Vital" />
      <Uid> <Fields.0 Value="1448301654" /> ... <Fields.3 Value="0" /> </Uid>
```

- `ProcessorState` is the same bytes a DAW gets from `IComponent::getState`: for Vital it starts
  exactly like the state REAPER saved for the same plug-in.
- The `Uid` fields are the four 32-bit numbers of the plug-in's id, as signed decimals
  (Fields.0 = 1448301654 = 0x56535456, "VSTV"). The same id REAPER writes as hex.
- **Live does not store the plug-in's file.** The bench finds it by id, like a DAW's scan: each
  `.vst3` is asked for the plug-ins it holds (`sessionscope_bench_host --list-classes`, its own
  process, 30 s watchdog; no plug-in is created) and the answer is cached in
  `%LOCALAPPDATA%\SessionScope\vst3-index.tsv`. Only 43 of the 600 `.vst3` files here ship the
  standard `moduleinfo.json` that would make this unnecessary. Files whose names share a word
  with the plug-in are asked first. The first run asked all 578 files in about two minutes (the
  UADx LA-2A's file, `uaudio_teletronix_la-2a_tc.vst3`, shares no word with its name).
- `<ParameterList>` keeps Live's own record of some parameters, but as **display values** (Gullfoss
  "Recover" 25.5 on its 0..200 scale = 0.1275 normalized; Ozone "Speed" 44.7 % = 0.447) or a
  placeholder 0.1234567 for parameters Live never read. 112 of 141 matched as they are; the
  checked differences are the same value in other units. Not used as a proof.

Code: `engine/src/inflate.*` (gzip, from RFC 1951/1952), `engine/src/als_reader.*`, and the
id index in `engine/src/bench_runner.*`. Tests: `test_inflate`, `test_als_reader`.
`sessionscope bench --project FILE.als` works like it does for `.rpp`.

## The bench on the set

48 kHz, 256-sample blocks, 5 s each, the person's settings:

| Track: plug-in | Check | Mean | p99 |
| --- | --- | ---: | ---: |
| 1-Vital | engaged | 8.50 % | 13.49 % |
| 2-Serum 2 | engaged | **42.60 %** | 70.65 % |
| 3: CHOWTapeModel | engaged | 5.24 % | 6.22 % |
| 4: ValhallaVintageVerb | engaged | 0.92 % | 1.40 % |
| 5: Pro-Q 4 | engaged | 0.91 % | 1.85 % |
| 6: Ozone 12 | engaged | **32.14 %** | 56.41 % |
| 7: soothe2 | engaged | 3.01 % | 6.84 % |
| 8: Gullfoss | engaged | 6.90 % | 9.66 % |
| 9: UADx LA-2A | engaged | 4.82 % | 6.10 % |
| 10-Kontakt 8 | silent | – | – |

At their defaults Serum 2 cost 3–4 % and Ozone 12 sat idle (`gauntlet.md`); with the person's
settings they are the two heaviest plug-ins in the set by far. Kontakt's saved state is small
(4 KB), which suggests no instrument is loaded in it; it is silent in the bench.

## The check: a second host reads Live's states

Live cannot list all of a plug-in's parameter values, so REAPER does it:
`hosts/reaper/tests/integration/als_states_into_rpp.ps1` takes the REAPER project from
`rpp-state.md` (the same ten plug-ins) and puts **Live's** saved states into it, keeping
REAPER's own header. The isolated REAPER opens it (`rpp_state_reopen.lua`) and writes down every
parameter value; the bench's values (`--parameters`, after the run) are compared with them.

| Plug-in | Parameters | Equal |
| --- | ---: | ---: |
| CHOWTapeModel | 54 | 54 |
| Vital | 2,852 | 2,852 |
| ValhallaVintageVerb | 18 | 18 |
| Serum 2 | 2,623 | 2,623 (values; see below) |
| Pro-Q 4 | 737 | 737 |
| Ozone 12 | 873 | 873 |
| soothe2 | 57 | 57 |
| Gullfoss | 10 | 10 |
| UADx LA-2A | 2,086 | 2,086 |
| Kontakt 8 | 4,145 | 4,145 |
| **All** | **13,455** | **13,455** |

Serum 2's eight macros have the person's preset's own names in the bench ("HARMONICS 2", "FM",
"TREMOLO"...) and REAPER's generic "Macro 1".."Macro 8" (REAPER keeps the names from its plug-in
scan); their values are equal.

## What this means

- The same bench, unchanged, measures a Live set and a REAPER project. Per DAW there is only a
  reader for its file format (REAPER: text and base64; Live: gzip XML and hex), because every
  DAW saves the same standard VST3 state.
- Open: VST2 plug-ins in either DAW (listed, not measured), Live's racks and their macros
  (devices inside racks are read; the rack's own mapping is not modelled), Audio Units (macOS),
  and the calibration of the timings against live readings.
