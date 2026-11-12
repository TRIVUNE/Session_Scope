# First look at Ableton Live, and the DAW-neutral direction

- Date: 2026-11-12 (evening session with the person) · Host: Ableton Live 12 (the person's own
  copy), Windows · Status: **first look, not a spike.** One short session; nothing here was
  measured by script yet.
- Companion notes (outside the repository, in the agent's plan folder):
  `C:\Users\Theodor\.opencode\plan\session-scope-daw-neutral-plan.md`.

## What the person did

Put the SessionScope plug-ins to work in Live: the Probe loaded and hosted Serum, numbers were
compared with Live's own per-track CPU, and the bars in the HUD were checked against the
project.

## Findings

1. **The plug-ins were invisible in Live for a boring reason.** `sessionscope install` puts
   them in the per-user folder (`%LOCALAPPDATA%\Programs\Common\VST3`), but Live scans **only
   the shared folder** `C:\Program Files\Common Files\VST3` (seen in Live's
   `PluginScanner.txt`; also noted in `handoff/task-manager-plan.md`). On 11 Nov the shared
   folder had no SessionScope plug-in at all (an old Sensor from 30 Oct had been removed).
   The person still got the Probe running in Live, so a copy reached Live one way or another;
   the install needs fixing properly (admin install into the shared folder).
2. **The Probe in Live: every hosted effect crashes — a systematic wrapper bug, not a plugin
   quirk.** The person picked **ten different audio effects** in the Probe's picker; **every
   one crashed Live** (Serum 2 among them). Seven of seven effects had worked in REAPER the
   same day, so this is one code path that Live exercises differently from REAPER — most
   likely on the hosting load path (initialize and bus setup, state streams, or editor
   attach). A deterministic crash on a single path is very debuggable, and the fix should
   transfer to other VST3-only hosts (Cubase, Studio One). Next step: set
   `SESSIONSCOPE_PROBE_LOG=<path>` before starting Live, reproduce once with any effect, and
   read the log — it records every call the host made of the Probe. Until this is fixed, the
   Probe must be called unsafe in Live.
   **The separate design gap stands: there is no instrument Probe.** The Probe is an effect
   wrapper and refuses instruments by design (`native/vst3_probe/sessionscope_probe.cpp`),
   so a synth cannot be measured at all — and synths are usually the heaviest plug-ins in a
   session. Building an instrument Probe is a separate wrapper (MIDI in → hosted synth →
   audio out, the same timing core), host-independent once built.
3. **The bars were wrong in Live.** Expected: the sensor reads the host's bar line (VST3
   `barPositionMusic`, slot 12); REAPER sends it exactly, Live apparently does not (or only in
   part), so the fallback (position + tempo, assuming 4/4) lies. To confirm and write down
   what Live actually sends: one `sessionscope record` + `report` during ~30 s of Live
   playback.
4. **The CPU numbers compared measure different things.** Live's per-track "5 %" is
   undocumented and known not to sum to reality. SessionScope has two different numbers: the
   DAW *process* share of the *whole machine* (one busy core on a 12–16-thread CPU shows as
   ~6–8 %) and the **Probe** number (time inside the plug-in ÷ block real-time length = the
   plug-in's share of the audio thread). "Live says 5 %, we say 1 %" is most likely a
   denominator mismatch, not a broken meter. Decisive calibration: 2 / 4 / 8 instances of the
   same plug-in — a work-measuring meter climbs in proportion.
5. **S2 is already answered (11 Nov, real hardware):** the callback-lateness proxy cannot warn
   and the Dropouts module lies on the Komplete Audio (749 false dropouts at 2 % load). The
   fixes are a decision about the *driver*, not about any DAW:
   `docs/research/S2-lateness.md`.

## The direction the person chose: DAW-neutral core

Remove everything DAW-dependent from the product promise; keep exactly the parts that need no
host cooperation:

- **Per-plug-in CPU** (the Probes): a stopwatch around the hosted plug-in's own process call —
  no host can refuse it. This is the centrepiece.
- **Machine health:** cores, busiest core, RAM, disk, CPU clock cap, DAW process CPU. Reads
  the machine, ignores the DAW.
- **Honest dropouts** (after the S2 fix): about the audio driver's timing, DAW-independent.
  Proposed fix: (a) lateness measured against the driver's own learned callback pattern, plus
  (b) sustained "falling behind real time" as the dropout sign; (c) the REAPER extension's
  realtime CPU stays as a REAPER bonus. *Not yet formally confirmed by the person.*
- **Bars** stop being the metric. The heatmap keeps its sliding window as a **time** heatmap
  everywhere; bar lines are drawn only where the host sends them (REAPER, Cubase, CLAP
  hosts), and the HUD says "bars: not provided by this host" instead of wrong numbers.
- **The lateness proxy retires.** **Track names** stay best-effort ("Unknown track" where the
  host does not send them).

Product promise in those terms: *per-plug-in CPU, session headroom, dropout detection and
machine health in any DAW that loads VST3 or CLAP; bar lines and track names where the host
provides them.*

## The person's question: "one button to probe every plug-in?"

- **From inside a plug-in: no.** Plug-in APIs isolate plug-ins from the project; no VST3/CLAP
  plug-in can enumerate, insert or rewire anything in the host.
- **Per-DAW scripting:** REAPER already has it (*SessionScope - Probe all FX* / *Remove
  probes*). Bitwig's deep extension API is the only other plausible candidate. Live, Cubase,
  FL and Studio One have no supported way for third parties to rewrite FX chains.
- **CLAP twins:** the zero-effort path in CLAP hosts (Bitwig, FL 21+, REAPER) — the DAW lists
  "*name* (SessionScope Probe)" next to every CLAP plug-in; pick the twin. Not available in
  Live or Cubase.
- **The one true single button, DAW-neutral: measure from outside.** A sampling profiler on
  the DAW process — sample the audio thread, attribute each sample to the DLL it lands in
  (standard technique; ETW `SampledProfile` on Windows needs no injection). One HUD toggle,
  any DAW. Limits: per-DLL, not per-instance (two Serums sum into one row); not block-accurate.
  A feasibility spike, not a promise.

Recommended layering: (1) HUD toggle "watch this DAW" — the outside sampler, per-DLL, one
click; (2) CLAP twins in CLAP hosts — per-instance, no workflow change; (3) the Probe itself —
inserted on the suspect(s) only, per-instance and block-accurate; plus the REAPER script where
REAPER is already the answer.

## The person's follow-up question: "insert one probe and find out what's wrong"

The person wants the simple flow: you suspect something wrong in the session, you insert one
Probe, and the tool tells you what is wrong. What one Probe can and cannot do:

- A Probe times **only what is inside it** — one insert = one plug-in. One Probe cannot audit
  a whole session; finding "who is hot" among many plug-ins needs the layering above (sampler,
  twins, or the REAPER script).
- But for the actual suspicion ("I think Serum is killing me"), **one insert is enough** if the
  Probe stops reporting numbers and starts reporting a **verdict**. Proposed "suspect flow"
  (not built yet — the person picks):
  - **A. A smarter Probe.** A freshly inserted Probe defaults to the last hosted plug-in
    (recents at the top of the picker), so the flow becomes: insert → one click → done.
  - **B. A verdict line in the HUD.** The Task Manager answers the question instead of listing
    numbers: "Serum — 41 % of the block on average, worst block 68 %; at a 128-sample buffer
    this alone can choke the audio thread", red past block-size-aware thresholds, green when
    fine. Honest about its limits: it measures the hosted plug-in, not the host's own overhead,
    and render-ahead can hide it (the open finding 2).
  - **C. Both** (recommended): A makes the insert cheap, B makes the answer readable.
- For "I don't know who the suspect is": step 1 of the layering (outside sampler in any DAW,
  or the REAPER button, or twins) finds the suspect; then one Probe confirms it precisely.

## Work order (from tonight, in order)

0. Admin-install the plug-ins into the shared VST3 folder; verify the Probe loads in Live from
   there.
1. One `record` + `report` in Live to write down what Live sends (bars, tempo, signature,
   track info).
2. Calibrate the Probe in Live: 2/4/8 instances of the same plug-in; compare with Live's track
   meter.
3. Build the S2 dropout fix (a) + (b); validate against `traces/s2-real-2026-11-11/`.
4. Bars → time axis + optional overlay; "not provided" instead of wrong.
5. Feasibility spike: the outside sampler (per-DLL CPU on the DAW's audio thread).
6. Write the honest support matrix and the product promise; the formal decision record
   (`docs/decisions/0006-mvp-path.md`) follows when work resumes.

## Guardrails (unchanged)

Never touch the person's own `hud.ini`; close the DAW before building; warnings are errors;
every test must pass; ask before building something new; commit after each phase.
