# Scouting the offline session analyzer (web research, 12 Nov 2026)

- Status: research before milestone 1. Sources checked the same evening; links inline.
- Question: with decision 0006 chosen, what will actually happen — does the offline approach
  work, does prior art exist, where does it break?

## What the research found

1. **The offline bench technique already exists and is validated.** [PlugPerf](https://github.com/ledraw1/pluginperf)
   (MIT, JUCE-based CLI, 26 commits, 8 stars) benchmarks a `.vst3` headless: mean/median/p95
   processing time per buffer size, buffer sweeps, CSV out, stability warnings, and a claimed
   **~95 % cross-validation agreement against a commercial in-DAW benchmark tool** at matching
   rates and buffers. This is prior art *for the measurement layer* — and strong evidence that
   offline numbers can match in-DAW numbers closely.
   - It does **not** do what decision 0006 adds: project-file parsing, the user's own saved
     states, the project's own MIDI and audio, per-bar maps, trace replay, what-if re-runs, or
     session-level analytics. Its framing is a developer tool ("test your plugin"), not a
     session analyzer. No competing product found for the session-analyzer framing.
2. **Headless hosting is established practice.** [headless-plugin-host](https://github.com/cyrillsemenov/headless-plugin-host)
   runs CLAP/VST3/AU headless as a CI GitHub Action (macOS/Windows/Linux) — the vendor world
   already runs plug-ins without a DAW routinely.
3. **The DRM hole is real, documented — and subtler than a crash.** A working group's own
   notes on iZotope Ozone ([ship-studios docs](https://github.com/Blankenship-Daniel/ship-studios/blob/main/docs/vst/izotope-ozone.md))
   record the "iLok-blocked-headless reality" and, worse, a **silent non-engagement finding**:
   the plug-in loaded headless but its processing did not engage. That failure mode gives
   *wrong numbers quietly*, which is exactly the kind of inaccuracy that kills a tool (see the
   Dropouts lesson, `docs/research/S2-lateness.md`).
4. **Standalone VST hosting is normal usage.** Widely documented guides (e.g.
   [guitargearfinder](https://guitargearfinder.com/guides/run-vst-plugins-standalone/)) run
   licensed plug-ins in third-party hosts daily; iLok machine licenses live on the machine.
   The iLok *cloud* workflow is the case that can block headless use.
5. **The project-file parsers mostly exist.** For `.als` (gzipped XML, Live 9–12):
   [pyableton](https://github.com/maranedah/pyableton) (PyPI), [als-parser](https://github.com/jbremz/als-parser)
   — which explicitly extracts *VST plug-in presets from the set*, i.e. the state-blob
   extraction our bench needs is proven feasible — plus several gists and a node library.
   `.rpp` is plain text; `.flp` has community parsers (pyflp). The parser work is integration,
   not invention.

## What will happen (prediction, to be checked by milestone 0.5)

- **The gauntlet (ten installed plug-ins, headless, with saved presets, verified against
  REAPER's extension numbers):** expect roughly **7–9 of 10 to run headless and agree within
  single-digit percent** — PlugPerf's ~95 % agreement and CI-hosting practice support this.
  Expect the **DRM'd minority to fail in one of two ways**: refuse outright (fine — flagged),
  or **load but silently not engage** (the Ozone case — must be caught, not trusted).
- **Design change the research forces, folded into the gauntlet and the bench:** every
  measurement must include an **engagement check** — push a test signal through, verify the
  output actually changes, and mark any plug-in that loads but does not engage as
  *unmeasurable (requires its licensed host)* rather than reporting a number. A plausible
  wrong number is the one failure mode this product cannot afford.
- **Where offline differs from live, honestly:** single-instance measurement does not model
  the DAW's core distribution, render-ahead, or shared-cache effects; calibration quantifies
  the gap per plug-in, and rankings are expected to survive even where percentages shift.
- **Sequencing consequence:** the gauntlet moves **before the parsers** — it uses the existing
  offline host and manually exported presets, needs no format work, and can redirect the plan
  (per decision 0006's review-when) for roughly a day's cost.

## Verdict

The chosen path survives contact with prior art: the risky part is proven (offline timing
matches), the parsers are mostly written by the community, the DRM gap is known and has a
detection design — and the *session-context* layer that would make SessionScope more than
PlugPerf remains unclaimed. Build the gauntlet first.

## Adopted into decision 0006 (same evening)

Everything above was folded into the accepted decision record
[`docs/decisions/0006-offline-session-analyzer.md`](../decisions/0006-offline-session-analyzer.md):

- The gauntlet became **milestone 0.5** and moved **before** any parser work.
- The **engagement check** is the defense against the silent-non-engagement case: a rich test
  signal in, the output must actually change, a suspicious pass-through gets the
  **parameter-nudge second test** (the project's own bypass state is honoured first), and every
  plug-in runs in its **own separate process** with a watchdog and timeout.
- Plug-ins that refuse to load or only engage inside their licensed host are reported
  **unmeasurable — requires its licensed host**, never a guessed number.
- Later items: the opt-in **community faithfulness database** (plug-in, version → faithful
  offline / needs its host) and the **outside per-DLL sampler** as the live complement for the
  plug-ins that refuse to run offline at all.
- The tool **never fights the DRM**: it measures what the person's licenses allow, detects
  what is faking, and tells the truth about the rest.
