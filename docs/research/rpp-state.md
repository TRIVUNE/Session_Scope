# Plug-in state from a REAPER project: does the bench get the person's exact settings?

- Date: 2026-11-12 · REAPER 7.83 (an isolated copy with its own settings folder) · Status:
  **passed.** 13,455 of 13,455 parameters of ten plug-ins equal.
- Why it matters: the person's presets are all in each vendor's own format (`.ffp`,
  `.SerumPreset`, `.vpreset`...), which no outside host can open (`gauntlet.md`, second pass).
  Decision 0006 does not need them: a project file already holds every plug-in's state in the
  standard VST3 form (`getState`), whatever the plug-in's own format. This record checks that.

## How REAPER stores a VST3 plug-in

Inside a track's `<FXCHAIN`, after a `BYPASS bypassed offline 0` line:

```
<VST "VST3: Pro-Q 4 (FabFilter)" "FabFilter Pro-Q 4.vst3" 0 "" 1234{ED57BD725C60467EA64DD2F400758B6F} ""
  base64 lines (at most 128 characters each)
>
```

- The 6th word holds the plug-in's id: the four 32-bit numbers the plug-in declares
  (`FUID l1 l2 l3 l4`) as 32 hex digits. Not the 16 bytes in memory order: on Windows those differ.
- Every base64 line decodes on its own (its length is a multiple of 4); joined, the bytes are:
  1. REAPER's header: u32 id, u32 magic, u32 input pins + 8 bytes per pin, u32 output pins + 8
     bytes per pin, u32 size of part 2, u32 1, u32 flags.
  2. The state: u32 component size, u32 1, the component's state, u32 controller size, u32 1,
     the controller's state. Serum 2, Pro-Q 4 and Gullfoss had a controller half; the others
     an empty one.
  3. The preset name REAPER shows.
  All numbers little-endian. Worked out on ten plug-ins and checked against the person's own
  project (KClip3); the reader refuses any block whose sizes don't add up exactly.

Code: `engine/src/rpp_reader.*` (test: `engine/tests/test_rpp_reader.cpp`).
`sessionscope bench --project FILE.rpp` benches every VST3 plug-in in the project with its state.
VST2, JS and CLAP plug-ins are listed as not measured yet; bypassed and offline ones are skipped.

## The check

1. `hosts/reaper/tests/integration/rpp_state_make.lua`, in an isolated REAPER that sees only
   the ten gauntlet plug-ins: one per track, settings changed (CHOWTapeModel: factory program 10;
   the others: every fourth parameter moved), project saved.
2. `rpp_state_reopen.lua`: REAPER reopens the project and writes down every parameter value.
   This is the reference. The values right after setting them are not: a plug-in rounds a
   switch set to 0.65 up to "on" (1.00), moves a stepped control to its nearest step, and never
   saves its MIDI-CC mapping slots or its unassigned automation slots.
3. `sessionscope bench --project proof.rpp --parameters`: the bench host loads each plug-in by its
   id, gives the component its state, the same bytes to the controller (`setComponentState`), then
   the controller its own state, as a DAW does. It reads every parameter after the timed run
   (`parameters_after_run`).

## Result

| Plug-in | Parameters | Equal | Bench: mean share, 48 kHz / 256 |
| --- | ---: | ---: | ---: |
| CHOWTapeModel | 54 | 54 | 8.09 % (its program 10, "Clean Fat"; 8.05 % chosen directly) |
| Vital | 2,852 | 2,852 | 3.85 % |
| ValhallaVintageVerb | 18 | 18 | 0.55 % |
| Serum 2 | 2,623 | 2,623 | 3.23 % |
| FabFilter Pro-Q 4 | 737 | 737 | 0.37 %, now processing (0.02 % with no bands at its defaults) |
| iZotope Ozone 12 | 873 | 873 | idle settings (no module was switched on) |
| oeksound soothe2 | 57 | 57 | 1.50 % |
| Soundtheory Gullfoss | 10 | 10 | 7.14 % |
| UADx LA-2A | 2,086 | 2,086 | 4.58 % |
| Kontakt 8 | 4,145 | 4,145 | silent (no instrument loaded, as in REAPER) |
| **All** | **13,455** | **13,455** | |

## What went wrong on the way (and is fixed)

- **Read too early.** Read right after the state was loaded, the UADx LA-2A's Gain was 0.322
  against REAPER's 0.370. The plug-in applies its saved state only once it is switched on;
  after the run it reads 0.370. The bench now reads parameters after the timed run.
- **Read after the nudge.** Plug-ins that fail the engagement check get every parameter moved
  by half its range; a reading after that is not the saved state. The reading is now taken
  before any nudge.
- **The id from the wrong word.** The reader took the 5th word instead of the 6th, so the id
  was always empty and the bench silently took the first plug-in in each file. It only gave
  the right plug-in because the first one happened to be it in every file; with Serum 2 (two
  plug-ins in one file) that was luck. Caught by the unit test, fixed, re-run: same result.
- REAPER itself crashed when quitting with these ten loaded (0xC0000005), every time, after
  saving. One of the plug-ins crashes on unload in REAPER; the bench host never did.

## What this means

The person's presets don't have to be readable by us: whatever a plug-in's own format, the
project carries its state in the standard form, and the bench gives that state back exactly.
Open: Ableton's `.als` (the same idea, its own container), VST2 plug-ins (the person's
Neurography projects use mostly VST2 IEM plug-ins), and plug-ins whose state points to files
(Kontakt's instruments, samples).
