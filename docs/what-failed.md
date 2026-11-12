# What failed, and why

- Written 2026-11-12, covering 27 Oct to 12 Nov 2026, the end of the project (decision 0007).
- For anyone joining the project: the ideas we tried that did not work, why they did not, and
  what we did instead. Read this before you suggest one of them again.
- Every point links to the record that holds the numbers. This page only explains them.

## The short version

1. **A number that looks right but is wrong is worse than no number.** Most of what failed
   gave believable numbers that measured the wrong thing. Examples: the lateness proxy, the
   Dropouts count, bars counted in quarter notes, and comparing our CPU with Live's.
2. **REAPER is not a typical DAW.** For three weeks we tested only in REAPER. The first evening
   in Ableton Live broke the in-session product: crashes, wrong bars, invisible plug-ins, and
   a workflow the person rejected.
3. **Test timing on real hardware, never on a dummy device.** The dummy audio device runs on a
   timer and never misses a deadline, so it cannot show whether a timing idea works.
4. **A plug-in cannot see or change the rest of the session.** The VST3 and CLAP plug-in APIs
   keep each plug-in isolated, so any plan that needs one plug-in to control the session fails.
5. **The measuring tool must cost very little.** It runs on the computer whose audio deadline
   it is measuring.
6. **The product itself did not stand** (decision 0007, the last section below). Every approach
   met at most three of four requirements, and the four can't all be met: per-instance numbers
   in a running session exist only inside the DAW.

## Measurement ideas that failed

### Callback lateness as a measure of load (spike S2)

- **The idea.** The sensor sits on the master track and sees only its own audio callbacks. If
  the computer is overloaded, the callbacks should arrive late. "Late" means the time between
  two callbacks minus the length of a block. We hoped this would work in every DAW.
- **What happened.** On the dummy device the result meant nothing (see below). On a real
  interface (Komplete Audio 6 MK2, ASIO, 11 Nov), the proxy failed in three ways:
  - **It reads high with no load.** The driver does not deliver callbacks evenly every
    5.33 ms. It delivers them on a 4 ms grid: 4, 4, 8 ms, repeating. So every third callback
    looks 50 % late when nothing is running.
  - **With anticipative FX off, it goes the wrong way.** When a block takes longer than the
    4 ms grid, callbacks queue up and arrive back to back, so the "late" third disappears. At
    103 % load, while the audio is breaking up, the proxy shows its lowest value: 4 %.
  - **With anticipative FX on (REAPER's default), it stays flat** until the audio breaks up.
    It reports the dropout after it happens; it never warns.
- **Why.** The time between callbacks is set by the driver, not by how hard the computer is
  working. Each driver has its own pattern.
- **Instead.** The proxy is retired. REAPER's real-time CPU, measured by our extension's
  audio hook, matches the known load within 1 point, but only in REAPER and only with
  anticipative FX off.
- Record: [`research/S2-lateness.md`](research/S2-lateness.md).

### The HUD's Dropouts module

- **The idea.** Count a block as a dropout when it is at least 40 % of a block late.
- **What happened.** With the driver pattern above, a session at 2 % load showed **749
  dropouts** and 0 % headroom.
- **Why.** It was built on the lateness proxy.
- **Instead.** Three fixes were written down: measure lateness against the driver's own
  pattern, treat "falling behind real time" as the dropout sign, or use the host's own count.
  The fix is paused because the live monitor itself is retired (decision 0006).

### The dummy audio device for timing tests

- **The idea.** The headless REAPER validation uses REAPER's dummy device. It needs no
  hardware, so it suited automated tests.
- **What happened.** At zero load, lateness p99 was already 23 to 192 % of a block. At 103 %
  load it still reported no xruns.
- **Why.** Its callbacks are paced by a timer, not a sound card. When processing is slow it
  simply waits, so it runs slower than real time instead of dropping audio.
- **Instead.** It is still used for tests that do not depend on timing (inventory, transport,
  bars, plug-in loading). Anything about timing runs on the real interface. The analyser flags
  traces recorded on the dummy device.

### REAPER's xrun counter as ground truth

- **What happened.** REAPER reported **0 xruns** at 103 % load in all four real-hardware
  traces.
- **Instead.** The known load injected by the Workload JSFX (fixtures F7a and F7b) is the
  ground truth. The extension's real-time CPU has been checked against it.

### Real-time CPU with anticipative FX on

- **What happened.** With REAPER's default setting, the audio thread looks idle (0.5 %) from 0
  to 94 % load, then the render-ahead buffer runs out and the audio breaks up.
- **Why.** Anticipative FX renders the tracks ahead of time on worker threads. The audio
  thread only collects the result.
- **Instead.** Nothing measured gives an early warning in this mode. No documented API for how
  full the render-ahead buffer is has been found.
- Record: [`research/D-timing.md`](research/D-timing.md).

### Per-plug-in and per-track CPU from REAPER scripts (spike D)

- **What happened.** Searching the whole ReaScript API found no function that returns CPU
  time. REAPER's Performance Meter is not available to scripts.
- **Instead.** The extension measures total real-time CPU. For per-plug-in numbers we built
  the Probe (see below).

### Bars counted from quarter notes

- **The idea.** The JSFX sensor knows the position in quarter notes and the current time
  signature, so it could work out the bar.
- **What happened.** Only 40.8 % of bars were right; everything from bar 6 onward was wrong
  after a time-signature change.
- **Why.** Using the current signature recounts the past as if it had always been in that
  signature.
- **Instead.** Use the bar start that the host sends (VST3 `barPositionMusic`, CLAP
  `bar_start`). REAPER sends it correctly. **Live showed wrong bars** on 12 Nov. The likely
  cause is that Live does not send the bar start (or sends only part of it), so our fallback
  assumes 4/4. This was never confirmed with a recording.
- Record: [`research/S1-bars.md`](research/S1-bars.md).

### Comparing our CPU number with Live's

- **What happened.** "Live says 5 %, SessionScope says 1 %" looked like a broken meter.
- **Why.** The numbers measure different things, so their denominators differ. Our DAW CPU is
  a share of the *whole machine*: one busy core on a 16-thread CPU is about 6 %. The Probe's
  number is time inside the plug-in divided by the block's length. Live's per-track number is
  not documented and is known not to add up.
- **Lesson.** Before comparing two CPU numbers, write down what each one is a share of. A fair
  check is 2, 4 and 8 instances of the same plug-in: a meter that measures real work rises in
  proportion.

## The in-session Probe: why it was retired

The Probe is a VST3 plug-in that holds another plug-in and times it. It worked in REAPER: seven
of seven third-party effects behaved the same with and without the Probe, including after save
and reopen. The first evening in Ableton Live (12 Nov) brought five problems at once.
Record: [`research/Live-first-look.md`](research/Live-first-look.md).

1. **It was not visible in Live.** `sessionscope install` copies the plug-ins to the per-user
   folder (`%LOCALAPPDATA%\Programs\Common\VST3`), but Live scans only the shared folder
   (`C:\Program Files\Common Files\VST3`), which needs an administrator install.
2. **Ten of ten hosted effects crashed Live.** The same build worked with seven of seven in
   REAPER that day, so the bug is probably on one code path that Live uses differently from
   REAPER, most likely loading (setup, buses, state or opening the editor). **It was never
   tracked down:** the Probe log (`SESSIONSCOPE_PROBE_LOG`) from a crash has not been read. If
   the Probe ever comes back, start there.
3. **Instruments cannot be measured.** The Probe is an effect wrapper and refuses instruments
   by design. Synths are usually the heaviest plug-ins in a session.
4. **The person's settings have to be moved by hand.** A Probe holds a *new* copy of the
   plug-in, so the settings the person made are not in it. REAPER's scripts can copy them over;
   no other DAW lets an outside tool do that. The person said: "if I have to migrate presets,
   then no." Two automatic fixes were considered and rejected:
   - **Pretending to be the original plug-in** (a Probe that uses the plug-in's own ID). This
     is impersonation: fragile, and it could damage the person's projects.
   - **Editing the project file to insert Probes.** This is file surgery on the person's work,
     different for every DAW, and one mistake can break a project.
5. **Every DAW behaves differently** (bars, track names, scan folders, threading, restarting
   plug-ins). The in-session product would need support for each DAW, and that list never
   ends.

Related ideas that failed for the same reason, plug-in isolation:

- **"One button that probes every plug-in", from inside a plug-in:** not possible. No VST3 or
  CLAP plug-in can list, insert or rewire anything in the host. Only scriptable hosts allow it
  (REAPER, maybe Bitwig).
- **CLAP "twins"** (a timed copy listed next to every CLAP plug-in): these work, but only in
  CLAP hosts. Live and Cubase do not load CLAP.

**What replaced it (decision 0006):** the offline session analyzer. It reads the project file,
loads each plug-in in SessionScope's own host in a separate process, with the plug-in's own
saved state, and times it there. Nothing is installed in the session, so nothing can crash the
DAW, and no settings need to be moved because they come from the file. The open risk is DRM:
a protected plug-in may refuse to run outside its DAW, or may load and **quietly not process
anything**, which would give a wrong number that looks normal. That is why the gauntlet comes
first and every measurement includes an engagement check.
Record: [`decisions/0006-offline-session-analyzer.md`](decisions/0006-offline-session-analyzer.md).

## Smaller failures along the way

| What we tried | What went wrong | What we do now |
| --- | --- | --- |
| HUD in Python + Electron (6 processes) | 88 % of one core and 470 MB, on the machine being measured | One C++ process: 16.5 % and 100 MB (decision 0005) |
| A richer, labelled HUD (legends, status lines, titles, logo) | The person found it "really bad": clutter that did not help a session | A plain strip of modules; a value instead of a sentence |
| MiniMeters' choice of the "high performance" GPU | On a laptop with two GPUs: about 52 fps, 14 ms per present | The GPU that drives the window's screen: 120 fps |
| A right-click menu drawn with ImGui | Cut off by the thin strip window | Real Windows menus |
| Filling the Probe's picker by loading every plug-in | Loading a few hundred plug-ins froze the window and risked a crash | List the `.vst3` files by name; load only the chosen one |
| Asking REAPER to restart the Probe after a plug-in is picked | REAPER does not restart a plug-in that asks to be restarted | The Probe sets up and starts the hosted plug-in itself (`start_now`) |
| The Sensor's default answer to "save your state" | "Not implemented" made Live log an error on every save | It answers "done" for its empty state |
| Tests that backed up and restored the real `hud.ini` | A second test overwrote the backup and the person's settings were lost | Tests pass `--settings <scratch file>`; screenshots touch no settings |
| Moving the mouse from a script to test drags | At 200 % display scaling, Windows rescales the script's mouse, so it cannot aim | `--test-drag` and `--test-wheel` inside the HUD |
| Engagement check moving only the first 16 parameters | Pro-Q 4 and Ozone 12 stayed bit-exact and looked "blocked" | Move every automatable parameter (up to 512): both respond (`research/gauntlet.md`) |
| Loading the person's presets from their files | Every vendor has its own format (`.ffp`, `.SerumPreset`, `.vpreset`...); only the plug-in's own window opens them | Take the state a DAW saved in the project: the standard VST3 form, whatever the vendor (`research/rpp-state.md`) |
| Reading a plug-in's parameters right after giving it its state | UADx LA-2A showed Gain 0.322 instead of 0.370: it applies its state only once switched on | Read them after the run (and before any nudge) |
| Comparing with the values a script had just set in REAPER | Plug-ins round switches and steps, and don't save MIDI-CC slots: hundreds of false differences | Compare with REAPER after reopening the saved project |
| Taking the VST3 id from the 5th word of `<VST ...>` | The id was always empty; the bench silently used the first plug-in in each file | The 6th word, read as four 32-bit numbers; a unit test holds it |
| Shell heredocs to write files | Backslashes (`\n`, `\x`) get mangled; again on 12 Nov in a Lua script (REAPER showed "invalid escape sequence") | Write files with the editor tools, always |
| REA with Ghidra 12.2 | REA rejects it; large executables timed out at Ghidra's default start-up limit | Ghidra 12.1.4, start-up timeout raised to 30 minutes |

## The last failure: the product (12 Nov 2026)

The offline bench worked. It loaded all ten test plug-ins headless, copy-protected ones included,
and gave each one the person's exact settings from a REAPER project and from an Ableton Live set
(13,455 of 13,455 parameters equal in both; `research/rpp-state.md`, `research/als-state.md`).
It showed what no DAW meter shows: with the person's settings Serum 2 cost 42.6 % of the block
and Ozone 12 32.1 %, against 3–4 % and idle at their defaults. Then the product question was
asked again, and it did not survive. Record: [`decisions/0007-stop.md`](decisions/0007-stop.md).

**The four requirements.** The person set them over two days, and each is reasonable:

1. **No DAW dependence**: no list of supported DAWs (the Probe's lesson).
2. **No carrying settings over**: "if I have to migrate presets, then no."
3. **Nothing of ours inside the DAW**: a tool that can crash a session is out.
4. **More than the DAW already shows**: REAPER's Performance Meter is exact per instance.

**How each idea fell, in the order they came up:**

- **Project readers** (built, worked): one reader per DAW is a support list again (1). They
  are small and can't crash a session, but "works with REAPER and Live" is still a list.
- **Our own plug-in rack**, where you open your plug-ins in SessionScope and pick presets in
  their own windows: the person saw at once that this is the Probe's dealbreaker again (2).
  The settings move by hand, just to a different place.
- **An outside sampler** watching the DAW process: it meets 1–3 but sees only plug-in *files*,
  not instances. Three Serums are one row, and REAPER already does better (4).
- **A timing hook inside the DAW**: per instance in any DAW, but it is our code in the DAW (3),
  and copy-protected plug-ins may see it as tampering.

**Why no fifth idea fixes it:** which *instance* of a plug-in is running is known only inside
the DAW process. Live, per-instance numbers need the DAW's own meter (4) or code inside the DAW
(3). Measuring outside the session needs the person's settings, which live only in per-DAW
project files (1) or must be entered again (2).

**What we should have done earlier:** written the requirements down on day one and tested every
idea against all of them before building it. We built for weeks before the first test outside
REAPER, and the requirements only became clear one failure at a time. And we should have
compared against what the DAWs already show (REAPER's per-instance meter) before deciding
SessionScope's measurement would be the selling point.

## How to avoid repeating these

- **Before building, write down what the product must do, and check each idea against all of
  it**, including "is this more than the DAW already does?"
- Before trusting a new measurement, test it against a **known load** (F7a/F7b), on **real
  hardware**, and in **more than one host**.
- Write down the denominator of every percentage.
- When a measurement cannot be trusted, show "unmeasurable" or "not provided", never a guess.
- Before designing a feature, check whether the plug-in API allows it at all.
- Keep the tool light: measure its own cost (spike E) every time something is added.
