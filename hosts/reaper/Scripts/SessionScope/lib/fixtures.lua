-- Fixture projects F1-F5 and F7 (blueprint appendix C), described as data and built through the
-- documented API, so the versioned fixture is this file rather than hand-edited .rpp text.
-- F6 (a corrupted or interrupted stream) is a trace fixture: `sessionscope simulate` (engine/src/simulate.cpp).
--
-- A fixture spec:
--   id, title, bars          name, description, length in bars
--   tempo = {{bar, bpm, num, den}, ...}   tempo/time signature markers (num/den optional after the first)
--   loop = {from, to}        loop range in bars (to is the first bar after it); repeat_on turns repeat on
--   realtime = true          anticipative FX off on every track and the master
--   master = {fx, ...}       FX on the master track
--   tracks = {{name, tone = Hz | chords = {{first bar, last bar, voices, lowest pitch}, ...},
--              arm, monitor, input, input_fx = {...}, fx = {...}, sends = {track name, ...}}, ...}
-- An fx is {name = 'ReaEQ'} or {js = 'Workload'} plus optional params (slider values in order;
-- {bar = n} means "the quarter-note position of bar n"), enabled = false, offline = true,
-- or {container = {fx, ...}}.

local json = require('json')
local util = require('util')
local wav = require('wav')

local M = {}

M.VERSION = 1
M.SRATE = 48000              -- every fixture runs at 48 kHz
M.INPUT_FX = 0x1000000       -- added to an FX index to address a track's input FX chain
M.CONTAINER = 0x2000000      -- added to an FX index to address an FX inside a container

-- How TrackFX_AddByName finds the SessionScope JSFX: by the name shown in the FX browser, or by path.
M.JS = {
  Sensor = { 'JS: SessionScope Sensor v0 (REAPER stand-in)', 'JS:SessionScope/SessionScope Sensor.jsfx' },
  Workload = { 'JS: SessionScope Workload (test load, fixture F7)', 'JS:SessionScope/SessionScope Workload.jsfx' },
  ['Fixed Latency'] = { 'JS: SessionScope Fixed Latency (test fixture F4)', 'JS:SessionScope/SessionScope Fixed Latency.jsfx' },
}

local SENSOR = { js = 'Sensor' }
local function bar(n) return { bar = n } end   -- resolved to the quarter-note position of bar n

-- F7a and F7b differ only in anticipative FX (REAPER's ahead-of-time rendering).
local function staircase(id, title, realtime)
  return {
    id = id, title = title, bars = 24, realtime = realtime,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 } },
    loop = { from = 1, to = 25 }, repeat_on = true,
    master = { SENSOR },
    -- +500 us per 256 samples every two bars: 0% of the deadline in bars 1-2, about 103% in bars 23-24 at 48 kHz.
    tracks = { { name = 'Workload staircase', fx = { { js = 'Workload', params = { 0, 1, 0, 0, 500, 8 } } } } },
  }
end

M.list = {
  {
    id = 'F1', title = 'Five audio tracks, no plugins: baseline sensor cost', bars = 32,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 } },
    master = { SENSOR },
    tracks = {
      { name = 'Tone 110 Hz', tone = 110 }, { name = 'Tone 220 Hz', tone = 220 }, { name = 'Tone 330 Hz', tone = 330 },
      { name = 'Tone 440 Hz', tone = 440 }, { name = 'Tone 550 Hz', tone = 550 },
    },
  },
  {
    id = 'F2', title = 'ReaSynth with rising polyphony and two serial FX', bars = 20,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 } },
    master = { SENSOR },
    tracks = {
      { name = 'Synth', chords = { { 1, 4, 1 }, { 5, 8, 2 }, { 9, 12, 4 }, { 13, 16, 8 }, { 17, 20, 16 } },
        fx = { { name = 'ReaSynth' }, { name = 'ReaEQ' }, { name = 'ReaComp' } } },
    },
  },
  {
    id = 'F3', title = 'Two independent chains and a shared reverb bus', bars = 16,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 } },
    master = { SENSOR },
    tracks = {
      { name = 'Chain A', tone = 220, sends = { 'Reverb Bus' }, fx = { { name = 'ReaEQ' }, { js = 'Workload', params = { 500 } } } },
      { name = 'Chain B', tone = 330, sends = { 'Reverb Bus' }, fx = { { name = 'ReaComp' }, { js = 'Workload', params = { 800 } } } },
      { name = 'Reverb Bus', fx = { { name = 'ReaVerbate' } } },
    },
  },
  {
    id = 'F4', title = 'Monitored vocal input with known plugin delay', bars = 8,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 } },
    master = { { js = 'Fixed Latency', params = { 256 } }, SENSOR },
    tracks = {
      { name = 'Lead Vox (armed)', arm = true, monitor = 1, input = 0,
        input_fx = { { js = 'Fixed Latency', params = { 64 } } },
        fx = {
          { name = 'ReaEQ' },
          { js = 'Fixed Latency', params = { 1024 } },
          { name = 'ReaComp' },
          { name = 'ReaEQ' },                                            -- duplicate name, distinct identity (T01)
          { js = 'Fixed Latency', params = { 512 }, enabled = false },   -- bypassed: does REAPER still report it?
          { js = 'Fixed Latency', params = { 128 }, offline = true },
          { container = { { js = 'Fixed Latency', params = { 256 } }, { name = 'ReaEQ' } } },
        } },
      { name = 'Guitar', tone = 196, fx = { { js = 'Fixed Latency', params = { 128 } } } },
    },
  },
  {
    -- 64 bars, tempo changes at bars 17 and 49, extra load in bars 17-48 and more in 33-48.
    id = 'F5', title = '64 bars with tempo changes and a heavy stretch in bars 33-48', bars = 64,
    tempo = { { bar = 1, bpm = 120, num = 4, den = 4 }, { bar = 17, bpm = 128 }, { bar = 49, bpm = 100 } },
    loop = { from = 33, to = 49 },
    master = { SENSOR },
    tracks = {
      { name = 'Drums (tone)', tone = 110 },
      { name = 'Bass', chords = { { 1, 64, 1, 36 } }, fx = { { name = 'ReaSynth' } } },
      { name = 'Pad Synth', chords = { { 1, 16, 2 }, { 17, 32, 4 }, { 33, 48, 8 }, { 49, 64, 2 } }, sends = { 'Reverb Bus' },
        fx = { { name = 'ReaSynth' }, { name = 'ReaEQ' } } },
      { name = 'Load bars 17-48', fx = { { js = 'Workload', params = { 600, 1, bar(17), bar(49) } } } },
      { name = 'Load bars 33-48', fx = { { js = 'Workload', params = { 1500, 1, bar(33), bar(49) } } } },
      { name = 'Reverb Bus', fx = { { name = 'ReaVerbate' } } },
    },
  },
  staircase('F7a', 'Calibration staircase, anticipative FX on (REAPER default)', false),
  staircase('F7b', 'Calibration staircase, anticipative FX off on every track', true),
  {
    -- F8 checks bar mapping where the time signature changes (spike S1 limitation): a sensor that
    -- only knows quarter notes and the current signature miscounts bars after bar 5.
    id = 'F8', title = 'Time-signature changes: 4/4, 3/4 at bar 5, 7/8 at bar 9, 4/4 at bar 13', bars = 16,
    tempo = {
      { bar = 1, bpm = 120, num = 4, den = 4 },    -- bars 1-4: 4 quarter notes per bar
      { bar = 5, bpm = 120, num = 3, den = 4 },    -- bars 5-8: 3 quarter notes per bar
      { bar = 9, bpm = 120, num = 7, den = 8 },    -- bars 9-12: 3.5 quarter notes per bar
      { bar = 13, bpm = 120, num = 4, den = 4 },   -- bars 13-16: back to 4/4
    },
    loop = { from = 1, to = 17 }, repeat_on = true,   -- loops the whole song, so a wrap is recorded too
    master = { SENSOR },                               -- the JSFX sensor on the master, as everywhere
    -- Silent: no audio items, only a light known load so the trace has a workload column to look at.
    tracks = { { name = 'Light load', fx = { { js = 'Workload', params = { 200 } } } } },
  },
}

-- Chord tones above the lowest pitch, in semitones: chords stack these, up to 16 voices.
local INTERVALS = { 0, 7, 12, 16, 19, 24, 28, 31, 36, 40, 43, 46, 48, 52, 55, 58 }

-- Everything below talks to REAPER through `r`; the build log collects anything that did not work.

-- Quarter-note position where bar n starts (bars are 1-based, REAPER's measures 0-based).
local function bar_qn(r, proj, n)
  local _, qn = r.TimeMap_GetMeasureInfo(proj, n - 1)
  return qn
end

-- Time in seconds where bar n starts, through the project's tempo map.
local function bar_time(r, proj, n)
  return r.TimeMap2_QNToTime(proj, bar_qn(r, proj, n))
end

-- A param value: {bar = n} becomes bar n's quarter-note position, anything else stays as it is.
local function resolve(r, proj, v)
  if type(v) == 'table' and v.bar then return bar_qn(r, proj, v.bar) end
  return v
end

-- Add one FX to a track (or its input chain), set its sliders and state. Returns its address.
local function add_fx(r, proj, tr, f, input, log)
  local idx = -1
  local names = f.js and M.JS[f.js] or { f.name }
  for _, name in ipairs(names) do
    idx = r.TrackFX_AddByName(tr, name, input, -1)
    if idx >= 0 then break end
  end
  if idx < 0 then
    log('could not add ' .. names[1])
    return nil
  end
  local addr = (input and idx < M.INPUT_FX) and idx + M.INPUT_FX or idx
  for i, v in ipairs(f.params or {}) do r.TrackFX_SetParam(tr, addr, i - 1, resolve(r, proj, v)) end
  if f.enabled == false then r.TrackFX_SetEnabled(tr, addr, false) end
  if f.offline then r.TrackFX_SetOffline(tr, addr, true) end
  return addr
end

-- A REAPER 7 container: add it, then move each child in with the documented container addressing.
local function add_container(r, proj, tr, children, log)
  local c = r.TrackFX_AddByName(tr, 'Container', false, -1)
  if c < 0 then log('could not add a container') return end
  for k, child in ipairs(children) do
    local s = add_fx(r, proj, tr, child, false, log)   -- added at the end of the track chain first
    if s then
      -- Container address from the TrackFX_AddByName docs: 0x2000000 + subitem * (chain count + 1)
      -- + container position, both 1-based. Moving the FX there puts it inside the container.
      local dest = M.CONTAINER + k * (r.TrackFX_GetCount(tr) + 1) + (c + 1)
      r.TrackFX_CopyToTrack(tr, s, tr, dest, true)
    end
  end
  local ok, n = r.TrackFX_GetNamedConfigParm(tr, c, 'container_count')
  if not ok or tonumber(n) ~= #children then
    log(string.format('container holds %s FX, expected %d', ok and n or '?', #children))
  end
end

local function add_chain(r, proj, tr, list, input, log)
  for _, f in ipairs(list or {}) do
    if f.container then add_container(r, proj, tr, f.container, log) else add_fx(r, proj, tr, f, input, log) end
  end
end

-- A looped 2-second sine item from bar 1 to the end, at half volume (generated, not anyone's music).
local function add_tone(r, proj, tr, freq, length, media)
  local path = util.join(media, string.format('tone_%d.wav', freq))
  wav.sine(path, M.SRATE, 2, freq, 0.125)
  local item = r.AddMediaItemToTrack(tr)
  local take = r.AddTakeToMediaItem(item)
  r.SetMediaItemTake_Source(take, r.PCM_Source_CreateFromFile(path))
  r.SetMediaItemInfo_Value(item, 'D_POSITION', 0)
  r.SetMediaItemInfo_Value(item, 'D_LENGTH', length)
  r.SetMediaItemInfo_Value(item, 'B_LOOPSRC', 1)
  r.SetMediaItemInfo_Value(item, 'D_VOL', 0.5)
end

-- One chord per bar, retriggered every bar: {first bar, last bar, voices, lowest pitch}.
local function add_chords(r, proj, tr, chords, bars)
  local item = r.CreateNewMIDIItemInProj(tr, 0, bar_qn(r, proj, bars + 1), true)
  local take = r.GetActiveTake(item)
  for _, c in ipairs(chords) do
    local from, to, voices, base = c[1], c[2], c[3], c[4] or 48
    for b = from, to do
      local q0, q1 = bar_qn(r, proj, b), bar_qn(r, proj, b + 1) - 0.25
      local p0, p1 = r.MIDI_GetPPQPosFromProjQN(take, q0), r.MIDI_GetPPQPosFromProjQN(take, q1)
      for v = 1, voices do r.MIDI_InsertNote(take, false, false, p0, p1, 0, base + INTERVALS[v], 90, true) end
    end
  end
  r.MIDI_Sort(take)
end

-- Build one fixture into the (empty) project proj. Media goes into folder/media.
function M.build(r, proj, spec, folder, log)
  -- Sample rate and tempo map first: every bar position below depends on them.
  r.GetSetProjectInfo(proj, 'PROJECT_SRATE', M.SRATE, true)
  r.GetSetProjectInfo(proj, 'PROJECT_SRATE_USE', 1, true)
  local first = spec.tempo[1]
  r.SetCurrentBPM(proj, first.bpm, false)
  for _, m in ipairs(spec.tempo) do
    r.SetTempoTimeSigMarker(proj, -1, -1, m.bar - 1, 0, m.bpm, m.num or 0, m.den or 0, false)
  end
  r.UpdateTimeline()

  local length = bar_time(r, proj, spec.bars + 1)
  local media = util.join(folder, 'media')
  r.RecursiveCreateDirectory(media, 0)

  -- Tracks, their content and FX. Sends come afterwards, once every track exists.
  local by_name = {}
  for i, t in ipairs(spec.tracks) do
    r.InsertTrackAtIndex(i - 1, true)
    local tr = r.GetTrack(proj, i - 1)
    by_name[t.name] = tr
    r.GetSetMediaTrackInfo_String(tr, 'P_NAME', t.name, true)
    if t.tone then add_tone(r, proj, tr, t.tone, length, media) end
    if t.chords then add_chords(r, proj, tr, t.chords, spec.bars) end
    if t.arm then
      r.SetMediaTrackInfo_Value(tr, 'I_RECINPUT', t.input or 0)
      r.SetMediaTrackInfo_Value(tr, 'I_RECMON', t.monitor or 1)
      r.SetMediaTrackInfo_Value(tr, 'I_RECARM', 1)
    end
    add_chain(r, proj, tr, t.input_fx, true, log)
    add_chain(r, proj, tr, t.fx, false, log)
  end
  for _, t in ipairs(spec.tracks) do
    for _, dest in ipairs(t.sends or {}) do
      if by_name[dest] then r.CreateTrackSend(by_name[t.name], by_name[dest]) else log('no track called ' .. dest) end
    end
  end

  local master = r.GetMasterTrack(proj)
  add_chain(r, proj, master, spec.master, false, log)

  -- I_PERFFLAGS bit 2 = "no anticipative FX": the track renders on the audio thread, in real time.
  if spec.realtime then
    for _, tr in pairs(by_name) do
      r.SetMediaTrackInfo_Value(tr, 'I_PERFFLAGS', math.floor(r.GetMediaTrackInfo_Value(tr, 'I_PERFFLAGS')) | 2)
    end
    r.SetMediaTrackInfo_Value(master, 'I_PERFFLAGS', math.floor(r.GetMediaTrackInfo_Value(master, 'I_PERFFLAGS')) | 2)
  end
  if spec.loop then
    r.GetSet_LoopTimeRange2(proj, true, true, bar_time(r, proj, spec.loop.from), bar_time(r, proj, spec.loop.to), false)
  end
  r.GetSetRepeatEx(proj, spec.repeat_on and 1 or 0)
  return length
end

-- What was built, with REAPER's version and any problems: written next to the fixtures.
function M.manifest(r, results)
  return {
    schema = 'sessionscope.fixtures',
    schema_version = M.VERSION,
    built_utc = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    host = { name = 'REAPER', version = r.GetAppVersion(), os = r.GetOS() },
    fixtures = json.array(results),
  }
end

M.NEW_TAB, M.CLOSE_TAB = 40859, 40860   -- File: New project tab / File: Close current project tab

-- Builds each fixture in its own new project tab, saves it as <root>/<id>/<id>.rpp and closes the
-- tab (keep_open = true leaves the last one open). Returns the manifest, also written to root.
function M.build_all(r, root, specs, keep_open)
  local results = {}
  for n, spec in ipairs(specs or M.list) do
    local before = r.EnumProjects(-1)
    r.Main_OnCommand(M.NEW_TAB, 0)
    local proj = r.EnumProjects(-1)
    if proj == before then error('could not open a new project tab') end
    -- A default project template may add tracks or master FX to the new tab: start from nothing.
    for i = r.CountTracks(proj) - 1, 0, -1 do r.DeleteTrack(r.GetTrack(proj, i)) end
    local master = r.GetMasterTrack(proj)
    for i = r.TrackFX_GetCount(master) - 1, 0, -1 do r.TrackFX_Delete(master, i) end

    local problems = {}
    local folder = util.join(root, spec.id)
    r.RecursiveCreateDirectory(folder, 0)
    r.PreventUIRefresh(1)
    local ok, err = pcall(M.build, r, proj, spec, folder, function(msg) problems[#problems + 1] = msg end)
    r.PreventUIRefresh(-1)
    if not ok then problems[#problems + 1] = 'build failed: ' .. tostring(err) end

    local path = util.join(folder, spec.id .. '.rpp')
    r.Main_SaveProjectEx(proj, path, 8)
    results[#results + 1] = { id = spec.id, title = spec.title, file = spec.id .. '/' .. spec.id .. '.rpp', path = path,
      bars = spec.bars, problems = json.array(problems) }
    if not (keep_open and n == #(specs or M.list)) then r.Main_OnCommand(M.CLOSE_TAB, 0) end
  end
  local manifest = M.manifest(r, results)
  util.write_file(util.join(root, 'manifest.json'), json.encode(manifest, '  ') .. '\n')
  return manifest
end

return M
