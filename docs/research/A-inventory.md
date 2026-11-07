# Spike A: project inventory

- Date: 2026-10-27 · Host: REAPER 7.77 x64, Windows 11 · Status: **passed on fixtures**

## Question

Can a script list tracks, FX, stable identities, bypass and offline state, input/monitoring FX
and container contents correctly (blueprint 10.4 A, test T01)?

## Setup

`lib/inventory.lua`, run by `SessionScope - Inventory.lua` and by the headless validation on
fixture F4 (built by `lib/fixtures.lua`): an armed vocal track with an input FX, seven top-level
FX including two ReaEQ instances, one bypassed and one offline JSFX, and a REAPER 7 container
holding two FX; a guitar track; a delay FX on the master.

## Observed

- All 7 fixtures (F1-F5, F7a, F7b) built through the API with no problems: stock plugins
  (ReaSynth, ReaEQ, ReaComp, ReaVerbate), the three SessionScope JSFX, sends, tempo markers,
  regions, MIDI and audio items, `I_PERFFLAGS`.
- The container was filled using the documented address formula. REAPER returned the child at
  `0x2000000 + 15` (= 1 × (7 + 1) + 7), as the formula predicts, and `container_count` = 2.
- Input FX are addressed as `0x1000000 + i` and listed separately; on the master the same chain
  holds monitoring FX.
- The two ReaEQ instances have the same name and different FX GUIDs.
- Capability flags derived at run time: transport.musical, project.inventory, metric.fx_pdc,
  metric.block_timing (sensor found on the master). host_rt, track_time and fx_time: no.

## Limitations

- Item and take FX are not listed (the blueprint allows v1 to scope to track inserts).
- Send destinations come from `GetTrackSendInfo_Value(..., "P_DESTTRACK")`; checked in the fake
  host only.

## Identity stability (validation run 4, 30 Oct 2026, REAPER 7.83)

On a copy of F4 the validation renamed an FX (`renamed_name`) and its track, moved an FX within
the chain, copied one, moved one to another track, moved the tracks around, saved, and reopened.
All 10 checks passed:

- FX GUIDs survive rename, reorder within a chain, a move to another track, and save + reopen.
- A copied FX gets a new GUID; its source keeps its own.
- Track GUIDs survive rename, track reorder, and save + reopen.

So FX GUID + track GUID is a stable identity for before/after comparisons. Not covered: copies
between projects, and track templates (both expected to give new GUIDs, like a copy).

## Result

Passed on the fixtures. Inventory is usable for P2.
