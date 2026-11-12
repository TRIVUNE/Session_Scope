# 0006: The offline session analyzer becomes the product; the in-session plug-ins step back

- Date: 2026-11-12
- Author: the person (decided in the evening session), written down by the agent
- Status: accepted

## Problem

The product as built asks the person to put SessionScope plug-ins inside their DAW: a sensor on
the master track, Probes wrapped around plug-ins, twins instead of originals. The first real
test outside REAPER (Ableton Live, 12 Nov 2026) showed what that costs:

- **Ten out of ten hosted effects crashed Live** while seven of seven had worked in REAPER the
  same day (`docs/research/Live-first-look.md`). A tool that can crash a DAW mid-session is
  disqualified, whatever its numbers say.
- **No instrument Probe exists**, so synths — usually the heaviest plug-ins in a session —
  cannot be measured at all.
- **The workflow fights the musician**: a probe holds a *fresh copy* of the plug-in, so the
  person's own settings must be carried over by hand in every host except REAPER. The person
  named this a dealbreaker: "if I have to migrate presets, then no."
- **Every host behaves differently** (bars, track names, scanning folders, threading), so the
  in-session product inherits a support matrix that is never finished.

Meanwhile the questions a musician actually asks — *"what is maxing my DAW? which plug-in is
eating what? where in the song does it choke?"* — are answerable another way: the **project
file already contains the plug-in list, each plug-in's exact saved state, the MIDI content per
bar, and the audio file references**. Loading those plug-ins in SessionScope's own offline host
(`native/vst3_probe/hosted_vst3.h` — already built and validated) and timing them with the
existing stopwatch produces measured numbers **without anything installed in the session and
nothing that can crash it**.

## Evidence

- Live session findings of 12 Nov 2026: `docs/research/Live-first-look.md` (ten-of-ten crash,
  the missing instrument Probe, the state-migration dealbreaker, the CPU-denominator
  confusion).
- REAPER validation of 11 Nov 2026: the Probe and hosting core pass (`handoff/task-manager-plan.md`),
  the extension tracks a known load within one point (S2, `docs/research/S2-lateness.md`) —
  that is the ground truth the offline bench will be calibrated against.
- The offline hosting core already exists and runs plug-ins outside any DAW
  (`native/vst3_probe/test_hosted_vst3.cpp`).

## Alternatives considered

1. **Keep the in-session product and fix it per host** (fix the Live crash, build the VST3
   twin factory, build an instrument Probe, keep chasing host quirks). Rejected: the workflow's
   core objection — your running plug-in cannot be adopted, its state must migrate by hand —
   has no legitimate automatic fix outside scripted hosts (see decision discussion of 12 Nov;
   same-ID impersonation and project-file surgery were both considered and rejected), and every
   new host re-opens the crash surface.
2. **Live monitoring only** (machine-health HUD without plug-ins). Rejected as the *product*:
   useful, but the person's question is about plug-ins, and it leaves the measurement problem
   unsolved.
3. **Offline session analyzer** (chosen): read the project file, measure the plug-ins offline
   with their own saved settings and the project's own material, report in plain visuals.

## Decision

SessionScope's product direction is the **offline session analyzer**:

1. The person selects a project file; the tool reads it (parser per format: REAPER `.rpp` first
   — plain text — then Ableton `.als`).
2. Every plug-in in the project is loaded in **SessionScope's own offline host** — in a
   **separate process**, so a misbehaving plug-in can only crash the bench, never the session
   or the analyzer.
3. Each plug-in is fed **its own saved state** from the file (the exact patch the person made),
   driven by the project's own MIDI density per bar and, where referenced, its own audio
   files, and timed with the existing per-block stopwatch.
4. The report answers, in visuals: which plug-in costs what (ranked), where in the song the
   load sits, what is duplicated or wasteful, what changes with the buffer size (a cost curve
   per plug-in at 64/128/256/512).
5. **The bench writes its results as a SessionScope trace**, in the format the engine already
   reads, and the run is faster than real time — the timeline is processed as fast as the CPU
   allows, not paced by the clock. The existing HUD then plays it back with `--replay`, so the
   "live feeling" of the project — meters moving, the expensive bar lighting up, scrubbing —
   comes from replaying the simulation instead of building a second real-time engine. A true
   1× real-time simulation stays possible later (the same blocks, paced by the clock); that
   buys presentation, not accuracy.
6. **What-if mode grows out of the same machinery**: once a plug-in's saved state is loaded,
   its parameters are ordinary host-side values, so the simulation can change one ("unison
   16 → 8, the reverb swapped") and re-run the timeline — the before/after question answered
   without touching the person's session.
7. Numbers are only shown as **measurements** after the bench passes calibration against the
   known-load fixtures in REAPER (F7a/F7b and the extension, agreement within a few points per
   plug-in). Until then they are labelled *estimates*.
8. **The plug-in gauntlet comes before the parsers** (milestone 0.5): ten installed plug-ins —
   including DRM-protected ones — run headless in the offline host with manually saved presets,
   under the defenses below, and their numbers are compared against live ground truth (the
   REAPER fixtures and the extension). If the gauntlet fails, the direction is revisited for
   roughly a day's cost, before any parser work exists.
9. The **HUD is re-targeted as the analyzer app's interface.** Its current job — a live
   monitor of the running DAW session — is retired; its engine (Direct3D 11 rendering, the
   module framework, settings, replay support) is kept and becomes the app's HUD: the weight
   map, the suspects and waste lists, trace playback and the what-if comparisons. The S2
   Dropouts fix (a)+(b) is paused with the live monitor; revisit if it ever returns.
10. The **Probe is triaged to the code the app needs.** The offline hosting core
   (`native/vst3_probe/hosted_vst3.*`) and the per-block timing it already carries migrate
   from "VST plug-in" to "app": the same hosting and timing, now inside `sessionscope bench`.
   The VST3 plug-in shell (picker, editor), the CLAP probe and twins, and the REAPER
   *Probe all FX* scripts are retired from the product and the build (kept in the tree and
   history as research); the sensor likewise steps out.

### How the DRM hole is handled

A protected plug-in (iLok, activation, host checks) can fail in our offline host in ways that
must never become a number. The layered answer:

| Plug-in behavior | What the tool does |
| --- | --- |
| Runs headless, processes faithfully | Measured, ranked |
| Loads but the signal passes unchanged (and the project says it is **not** bypassed) | Parameter-nudge test; still silent → **unmeasurable, flagged** |
| Bypassed in the project | "Cost ≈ 0 (bypassed)" — the file states the intent |
| Crashes, hangs, or opens a license dialog | **Separate process + watchdog + timeout**: that process dies, "failed to load" is logged, the analyzer and the person's session are untouched |
| Works but only inside its licensed host (e.g. the iLok cloud case) | **Unmeasurable — requires its licensed host**; the report states its share of the session's unmeasured load; never a guessed number |

The engagement check is the heart of it: a rich test signal (noise + clicks) goes in, and the
output must actually change; a legit pass-through at the project's settings is told apart from
a silent shell by the parameter-nudge test. Longer term, opt-in results accumulate into a
**community faithfulness database** ("plug-in X, version Y: faithful offline / needs its
host"), and the **outside per-DLL sampler** is the planned live complement for the plug-ins
that refuse to run offline at all. The tool never fights the DRM: it measures what the
person's licenses allow, detects what is faking, and tells the truth about the rest.

## Consequences

- **Engineering:** the new work is the project-file parsers, the bench command
  (`sessionscope bench <project>`), the calibration harness against the REAPER fixtures, and
  the visual report. The hosting core, timing, engine, report pipeline and HUD already exist —
  the expensive research of P1 is reused, not discarded.
- **Crash risk moves from the person's session to our bench process** — acceptable and
  observable (`SESSIONSCOPE_PROBE_LOG` carries over to the bench host).
- **Limits, on the record** (`docs/research/Live-first-look.md`, capability table of 12 Nov):
  it is a photo, not a live feed; it cannot see the DAW's own overhead, the audio driver, or
  dropouts; DRM'd plug-ins may refuse to run outside their host; per-DAW work becomes
  per-*format* work, and a format quirk can never crash the person's session.
- **Support matrix changes shape**: instead of "tested in every DAW" the promise becomes
  "parsed formats": REAPER and Live first, others as parsers land.
- **Customer story, stated honestly**: v1 analytics (inventory, waste, expensive bar, ranked
  suspects) ships before v2 measured rankings; the product earns accuracy version by version
  and labels every estimate as one.

## Review when

- The calibration table shows the bench disagreeing with live measurements beyond a few points.
- A widely used plug-in proves unusable offline (DRM, headless refusal) — revisit the promise
  for those plug-ins.
- The person wants live per-plug-in truth again — the Probes and REAPER scripts remain in the
  tree, documented, ready.
