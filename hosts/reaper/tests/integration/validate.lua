-- Headless validation inside a real REAPER, started by `sessionscope reaper validate` (engine/src/reaper_tools.cpp) with an isolated
-- resource folder (-cfgfile), so the user's own REAPER setup is never touched.
--
--   1. builds every fixture (F1-F5, F7) through the real API and records any problems
--   2. F4: plays two seconds so the JSFX report their delay, then takes the inventory and
--      compares each Fixed Latency instance's declared delay with its slider (spike C)
--   3. F7b and F7a: records the calibration staircase once each (spike S2, anticipative off/on)
--   4. F5: records bars 13-20 and 45-52, a tempo change and a seek in each (spikes B and S1)
--   5. T10: two sensors on one master must be reported as such
--   6. identity: FX and track GUIDs across rename, reorder, copy, move and save + reopen (spike A)
--   7. F8: records the whole song across three time-signature changes (spike S1)
--   8. probe: a VST3 effect swapped for a SessionScope Probe holding it must be timed, live and
--      after save + reopen, and swap back (the Task Manager's data path, in a real host)
--   9. writes <resource>/SessionScope/validation/result.json and quits REAPER
--
-- Audible fixtures are only played when the audio device is REAPER's dummy device.

local here = debug.getinfo(1, 'S').source:match('^@(.*[/\\])')
local lib = here .. '../../Scripts/SessionScope/lib/'
package.path = lib .. '?.lua;' .. package.path

local fixtures = require('fixtures')
local inventory = require('inventory')
local json = require('json')
local probes = require('probes')
local recorder = require('recorder')
local util = require('util')

local r = reaper
local QUIT = 40004   -- File: Quit REAPER
local base = util.join(r.GetResourcePath(), 'SessionScope')
local out_root = util.join(base, 'validation')
r.RecursiveCreateDirectory(out_root, 0)

local result = {
  schema = 'sessionscope.validation', schema_version = 1,
  started_utc = os.date('!%Y-%m-%dT%H:%M:%SZ'),
  host = { name = 'REAPER', version = r.GetAppVersion(), os = r.GetOS(), resource = r.GetResourcePath() },
  device = {}, steps = json.array(), errors = json.array(), traces = {},
}
for _, k in ipairs({ 'MODE', 'SRATE', 'BSIZE' }) do
  local ok, v = r.GetAudioDeviceInfo(k)
  result.device[k:lower()] = ok and v or nil
end
result.device.audio_running = r.Audio_IsRunning() == 1
-- Set by the SessionScope extension when REAPER loaded it from UserPlugins (empty otherwise).
result.host_extension = r.GetExtState('SessionScope', 'host_ext')
local silent = (result.device.mode or ''):lower():find('dummy') ~= nil
result.device.silent = silent

local function note(step, data)
  data = data or {}
  data.step = step
  data.t = r.time_precise()
  result.steps[#result.steps + 1] = data
end

local function fail(step, err)
  result.errors[#result.errors + 1] = step .. ': ' .. tostring(err)
end

local function save_result()
  result.finished_utc = os.date('!%Y-%m-%dT%H:%M:%SZ')
  util.write_file(util.join(out_root, 'result.json'), json.encode(result, '  ') .. '\n')
end

local function open_fixture(manifest, id)
  for _, f in ipairs(manifest.fixtures) do
    if f.id == id then
      r.Main_OnCommand(fixtures.NEW_TAB, 0)
      r.Main_openProject(f.path)
      return r.EnumProjects(-1)
    end
  end
  error('fixture ' .. id .. ' was not built')
end

local function bar_time(proj, bar)
  local _, qn = r.TimeMap_GetMeasureInfo(proj, bar - 1)
  return r.TimeMap2_QNToTime(proj, qn)
end

-- A small sequencer over defer ticks: each job runs until its step function returns true.
local jobs, current = {}, nil
local function job(name, fn) jobs[#jobs + 1] = { name = name, fn = fn } end

local function after(seconds, fn)
  local t0
  return function()
    t0 = t0 or r.time_precise()
    if r.time_precise() - t0 < seconds then return false end
    return fn() ~= false
  end
end

local manifest

job('build fixtures', function()
  manifest = fixtures.build_all(r, util.join(base, 'fixtures'))
  local problems = {}
  for _, f in ipairs(manifest.fixtures) do
    for _, p in ipairs(f.problems) do problems[#problems + 1] = f.id .. ': ' .. p end
  end
  note('build fixtures', { built = #manifest.fixtures, problems = json.array(problems) })
  return true
end)

-- F4: let the JSFX run so they declare their delay, then read it back.
local f4
job('F4 open and play', function()
  f4 = open_fixture(manifest, 'F4')
  if silent then r.CSurf_OnPlay() end
  return true
end)
job('F4 inventory', after(2.5, function()
  r.CSurf_OnStop()
  local inv = inventory.collect(r, f4)
  util.write_file(util.join(out_root, 'F4-inventory.json'), json.encode(inv, '  ') .. '\n')
  local checks = json.array()
  local function walk(tr, list, where)
    for _, e in ipairs(list) do
      if e.name and e.name:find('Fixed Latency', 1, true) then
        local slider = r.TrackFX_GetParam(tr, e.address, 0)
        checks[#checks + 1] = { where = where, address = e.address, slider = slider, pdc = e.pdc_samples,
          enabled = e.enabled, offline = e.offline }
      end
      if e.container then walk(tr, e.container, where .. ' > container') end
    end
  end
  for i, t in ipairs(inv.tracks) do
    local tr = r.GetTrack(f4, i - 1)
    walk(tr, t.fx, t.name)
    walk(tr, t.input_fx, t.name .. ' (input FX)')
  end
  walk(r.GetMasterTrack(f4), inv.master.fx, 'MASTER')
  local params = {}
  local tr = r.GetTrack(f4, 0)
  for i = 0, r.TrackFX_GetNumParams(tr, 1) - 1 do
    local _, name = r.TrackFX_GetParamName(tr, 1, i)
    params[#params + 1] = name
  end
  note('F4 inventory', { played = silent, fixed_latency = checks, capabilities = inv.capabilities,
    fixed_latency_params = json.array(params), container_children = inv.tracks[1].fx[7] and inv.tracks[1].fx[7].container and #inv.tracks[1].fx[7].container or nil })
  return true
end))

-- Is a bypassed FX's delay reported as 0 because REAPER ignores it, or because a freshly loaded
-- JSFX never ran? Enable the bypassed one (slot 4, 512 samples), let it run, then bypass it again.
local BYPASSED = 4
local function pdc_of(proj)
  local _, v = r.TrackFX_GetNamedConfigParm(r.GetTrack(proj, 0), BYPASSED, 'pdc')
  return tonumber(v)
end
local bypass = {}
job('F4 bypass: enable', function()
  r.TrackFX_SetEnabled(r.GetTrack(f4, 0), BYPASSED, true)
  if silent then r.CSurf_OnPlay() end
  return true
end)
job('F4 bypass: bypass again', after(1.5, function()
  bypass.enabled = pdc_of(f4)
  r.TrackFX_SetEnabled(r.GetTrack(f4, 0), BYPASSED, false)
  bypass.just_bypassed = pdc_of(f4)
  return true
end))
job('F4 bypass: read', after(1.5, function()
  bypass.bypassed_after_1_5_s = pdc_of(f4)
  r.CSurf_OnStop()
  note('F4 bypass', { slider = 512, played = silent, pdc = bypass })
  -- Toggling bypass marks the project changed; it is back as built, so save rather than face a prompt.
  if r.IsProjectDirty(f4) ~= 0 then r.Main_SaveProject(f4, false) end
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
  return true
end))

-- Spike A, identity stability (appendix C): do FX and track GUIDs survive rename, reorder, a move
-- to another track and save + reopen, and does a copy get a new identity? Works on a copy of F4,
-- so the fixture itself stays as built.
local function fx_list(tr)
  local out = {}                                                   -- {name, guid} per top-level FX, in chain order
  for i = 0, r.TrackFX_GetCount(tr) - 1 do
    local _, name = r.TrackFX_GetFXName(tr, i)                     -- the name shown in the chain (renames included)
    out[#out + 1] = { name = name, guid = r.TrackFX_GetFXGUID(tr, i) }
  end
  return out
end

local function slot_of(tr, guid)
  for i = 0, r.TrackFX_GetCount(tr) - 1 do                         -- linear search: chains are short
    if r.TrackFX_GetFXGUID(tr, i) == guid then return i end
  end
  return nil                                                       -- not on this track
end

local ident = {}                                                   -- state shared by the identity jobs
job('identity: edit', function()
  local proj = open_fixture(manifest, 'F4')                        -- a fresh F4 tab
  ident.path = util.join(out_root, 'F4-identity.rpp')              -- saved under a new name, never over F4
  local vox, gtr = r.GetTrack(proj, 0), r.GetTrack(proj, 1)        -- the vocal and guitar tracks
  local before = fx_list(vox)                                      -- identities as built
  local c = {}                                                     -- the checks, written to result.json
  ident.vox_guid = r.GetTrackGUID(vox)                             -- track identity before any edit

  -- Rename: the FX's display name and the track name change; identities must not.
  r.TrackFX_SetNamedConfigParm(vox, 0, 'renamed_name', 'Renamed EQ')
  r.GetSetMediaTrackInfo_String(vox, 'P_NAME', 'Renamed Vox', true)
  c.fx_rename_keeps_guid = r.TrackFX_GetFXGUID(vox, 0) == before[1].guid
  c.fx_rename_shows_new_name = select(2, r.TrackFX_GetFXName(vox, 0)) == 'Renamed EQ'
  c.track_rename_keeps_guid = r.GetTrackGUID(vox) == ident.vox_guid

  -- Reorder: move FX 1 (the first ReaEQ) to slot 3; its GUID must follow it.
  r.TrackFX_CopyToTrack(vox, 0, vox, 2, true)
  c.fx_reorder_guid_follows = slot_of(vox, before[1].guid) == 2

  -- Copy: duplicate the ReaComp (originally slot 3, now slot 2 after the move) to the end of the chain.
  local comp = before[3].guid                                      -- ReaComp's GUID as built
  r.TrackFX_CopyToTrack(vox, slot_of(vox, comp), vox, r.TrackFX_GetCount(vox), false)
  local copy_guid = r.TrackFX_GetFXGUID(vox, r.TrackFX_GetCount(vox) - 1)
  c.fx_copy_gets_new_guid = copy_guid ~= nil and copy_guid ~= comp
  c.fx_copy_source_unchanged = slot_of(vox, comp) ~= nil

  -- Move to another track: the second ReaEQ (slot 4 as built) goes to the guitar's chain.
  local eq2 = before[4].guid
  r.TrackFX_CopyToTrack(vox, slot_of(vox, eq2), gtr, 0, true)
  c.fx_move_to_track_keeps_guid = slot_of(gtr, eq2) == 0 and slot_of(vox, eq2) == nil

  -- Track reorder: move the guitar above the vocal; both tracks keep their GUIDs.
  r.SetOnlyTrackSelected(gtr)
  r.ReorderSelectedTracks(0, 0)
  c.track_reorder_keeps_guid = r.GetTrackGUID(r.GetTrack(proj, 1)) == ident.vox_guid

  ident.expect_vox = fx_list(r.GetTrack(proj, 1))                  -- the vocal chain after all edits
  ident.expect_gtr = fx_list(r.GetTrack(proj, 0))                  -- the guitar chain after all edits
  ident.checks = c
  r.Main_SaveProjectEx(proj, ident.path, 8)                        -- save the edited copy ...
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)                          -- ... and close it
  return true
end)

job('identity: reopen', function()
  r.Main_OnCommand(fixtures.NEW_TAB, 0)                            -- reopen the edited copy in a new tab
  r.Main_openProject(ident.path)
  local proj = r.EnumProjects(-1)
  local function same(tr, expect)                                  -- same FX, same order, same GUIDs?
    local got = fx_list(tr)
    if #got ~= #expect then return false end
    for i, e in ipairs(expect) do
      if got[i].guid ~= e.guid or got[i].name ~= e.name then return false end
    end
    return true
  end
  local c = ident.checks
  c.reopen_keeps_fx_guids = same(r.GetTrack(proj, 1), ident.expect_vox) and same(r.GetTrack(proj, 0), ident.expect_gtr)
  c.reopen_keeps_track_guid = r.GetTrackGUID(r.GetTrack(proj, 1)) == ident.vox_guid
  local pass = true                                                -- every check must hold
  for _, v in pairs(c) do pass = pass and v end
  note('identity', { checks = c, passed = pass })
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
  return true
end)

local function record(id, plan)
  local proj, rec, t0, phase
  job(id .. ' record', function()
    if not proj then
      proj = open_fixture(manifest, id)
      if plan.audible and not silent then
        note(id .. ' skipped', { reason = 'audio device is not the dummy device; not playing an audible fixture' })
        r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
        return true
      end
      if plan.from then r.SetEditCurPos(bar_time(proj, plan.from), false, false) end
      rec = recorder.start(r, proj, util.join(out_root, id))
      r.CSurf_OnPlay()
      t0 = r.time_precise()
      phase = 1
      return false
    end
    rec.tick()
    local pos = r.GetPlayPosition2Ex(proj)
    if plan.seek_at and phase == 1 and pos >= bar_time(proj, plan.seek_at) then
      r.SetEditCurPos(bar_time(proj, plan.seek_to), false, true)
      phase = 2
    end
    local done = r.time_precise() - t0 >= plan.seconds or (plan.until_bar and phase == 2 and pos >= bar_time(proj, plan.until_bar))
    if not done then return false end
    r.CSurf_OnStop()
    rec.tick()
    local summary = rec.finish()
    result.traces[id] = { folder = rec.out, summary = summary }
    note(id .. ' recorded', { seconds = r.time_precise() - t0 })
    r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
    return true
  end)
end

-- T10: two sensor instances on the master of an empty project; the recorder must report it.
local t10 = {}
job('T10 setup', function()
  r.Main_OnCommand(fixtures.NEW_TAB, 0)
  t10.proj = r.EnumProjects(-1)
  local master = r.GetMasterTrack(t10.proj)
  for _ = 1, 2 do
    for _, name in ipairs(fixtures.JS.Sensor) do
      if r.TrackFX_AddByName(master, name, false, -1) >= 0 then break end
    end
  end
  r.Main_SaveProjectEx(t10.proj, util.join(out_root, 'T10.rpp'), 8)   -- saved, so closing does not ask
  t10.rec = recorder.start(r, t10.proj, util.join(out_root, 'T10'))
  r.CSurf_OnPlay()
  t10.t0 = r.time_precise()
  return true
end)
job('T10 record', function()
  t10.rec.tick()
  if r.time_precise() - t10.t0 < 4 then return false end
  r.CSurf_OnStop()
  t10.rec.tick()
  local s = t10.rec.finish()
  result.traces.T10 = { folder = t10.rec.out, summary = s }
  note('T10 two sensors', { second_sensor = s.second_sensor, records = s.sensor_records })
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
  return true
end)

-- The SessionScope Probe (native/vst3_probe) with a real plug-in inside. The plug-in is our own
-- VST3 Sensor: the one VST3 effect that is there on every computer that built this repository.
-- lib/probes.lua swaps it for a Probe holding it; `sessionscope probes --json` (the console
-- program, reading the probes' shared table as the HUD does) must then show the track with blocks
-- being timed: live, and again after save + reopen, which is how REAPER normally loads a Probe.
local SENSOR_VST3 = 'VST3:SessionScope Sensor (TRIVUNE)'
local SENSOR_CLASS = '5E55F0A17C0D4B6E9A2E1D430B5C8E77'               -- native/vst3_sensor: its class id
local probe = { checks = {}, path = util.join(out_root, 'probe.rpp') }
local function probes_seen()                                         -- track listed? how many blocks timed?
  local out = r.ExecProcess('"' .. here .. '../../../../build/engine/sessionscope.exe" probes --json', 10000) or ''
  return out:find('"name": "Probe test"', 1, true) ~= nil, tonumber(out:match('"blocks":%s*(%d+)')) or 0
end
job('probe: wrap', function()
  r.Main_OnCommand(fixtures.NEW_TAB, 0)
  probe.proj = r.EnumProjects(-1)
  r.InsertTrackAtIndex(0, true)
  local tr = r.GetTrack(probe.proj, 0)
  r.GetSetMediaTrackInfo_String(tr, 'P_NAME', 'Probe test', true)
  if r.TrackFX_AddByName(tr, SENSOR_VST3, false, -1) < 0 then
    probe.skipped = true
    note('probe skipped', { reason = 'REAPER does not list the VST3 Sensor (build it, or let REAPER rescan its plug-ins)' })
    return true
  end
  local ok, what = probes.wrap(r, tr, 0, true)                      -- true: the Sensor may be wrapped here
  probe.checks.wrap_succeeds = ok
  if not ok then probe.why = what end
  local held = probes.read_probe(r, tr, 0)
  probe.checks.probe_holds_the_plugin = held ~= nil and held.class_id == SENSOR_CLASS
  r.CSurf_OnPlay()
  return true
end)
job('probe: live', after(2.5, function()
  if probe.skipped then return end
  local listed, blocks = probes_seen()
  probe.checks.live_lists_the_track = listed
  probe.checks.live_times_blocks = blocks > 0
  r.CSurf_OnStop()
  r.Main_SaveProjectEx(probe.proj, probe.path, 8)
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
  r.Main_OnCommand(fixtures.NEW_TAB, 0)                            -- and open it again, as a user would tomorrow
  r.Main_openProject(probe.path)
  probe.proj = r.EnumProjects(-1)
  r.CSurf_OnPlay()
end))
job('probe: reopened', after(2.5, function()
  if probe.skipped then r.Main_OnCommand(fixtures.CLOSE_TAB, 0) return end
  local listed, blocks = probes_seen()
  probe.checks.reopened_lists_the_track = listed
  probe.checks.reopened_times_blocks = blocks > 0
  r.CSurf_OnStop()
  local tr = r.GetTrack(probe.proj, 0)
  local ok = probes.unwrap(r, tr, 0)                                -- and back: the plug-in itself again
  local _, name = r.TrackFX_GetFXName(tr, 0)
  probe.checks.unwrap_puts_the_plugin_back = ok and r.TrackFX_GetCount(tr) == 1 and name:find('SessionScope Sensor', 1, true) ~= nil
  local pass = true
  for _, v in pairs(probe.checks) do pass = pass and v end
  note('probe', { checks = probe.checks, passed = pass, why = probe.why })
  r.Main_SaveProjectEx(probe.proj, probe.path, 8)                   -- saved, so closing does not ask
  r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
end))

record('F7b', { seconds = 50 })
record('F7a', { seconds = 50 })
record('F5', { audible = true, from = 13, seek_at = 21, seek_to = 45, until_bar = 53, seconds = 60 })
record('F8', { from = 1, seconds = 34 })   -- the whole song (29 s) and a loop wrap, across three signature changes

job('finish', function()
  result.complete = true   -- every job ran: `sessionscope reaper validate` looks for this
  save_result()
  r.Main_OnCommand(QUIT, 0)
  return true
end)

local deadline = r.time_precise() + 540   -- 9 minutes: the recordings alone take about 3.5
local function tick()
  if r.time_precise() > deadline then
    fail('watchdog', 'validation took longer than 9 minutes')
    save_result()
    r.Main_OnCommand(QUIT, 0)
    return
  end
  current = current or table.remove(jobs, 1)
  if not current then return end
  local ok, finished = pcall(current.fn)
  if not ok then
    fail(current.name, finished)
    finished = true
  end
  if finished then
    current = nil
    save_result()
  end
  r.defer(tick)
end

save_result()
r.defer(tick)
