-- A small in-memory REAPER: a project of tracks with FX chains, input FX and containers, a 4/4
-- tempo map, items, markers and transport, behind the same function names as the real API.
-- Only what the SessionScope modules call is implemented; anything else raises an error.

local M = {}

local INPUT, CONTAINER = 0x1000000, 0x2000000

local function fx(name, o)
  o = o or {}
  return {
    name = name, guid = o.guid or ('{' .. name .. '}'), enabled = o.enabled ~= false, offline = o.offline or false,
    pdc = o.pdc, type = o.type or 'VST3', ident = o.ident, params = o.params or {}, children = o.children,
    instrument = o.instrument,
  }
end
M.fx = fx

function M.new(o)
  o = o or {}
  local P = {
    tracks = {},
    master = { name = 'MASTER', guid = '{MASTER}', fx = {}, recfx = {}, info = { I_FXEN = 1, I_PERFFLAGS = 0 }, sends = {} },
    markers = {}, tempo = {}, info = {}, items = {}, bpm = o.bpm or 120,
    play_state = 0, pos = 0, pos2 = 0, cursor = 0, rate = 1, repeat_on = 0, loop = { 0, 0 },
    installed = o.installed,       -- names TrackFX_AddByName can find; nil = everything
    calls = {},
  }
  local r = { _p = P }
  setmetatable(r, { __index = function(_, k) error('fake REAPER has no ' .. tostring(k), 2) end })

  local function track(name)
    return { name = name, guid = '{' .. name .. '}', fx = {}, recfx = {}, info = { I_FXEN = 1, I_PERFFLAGS = 0, I_RECARM = 0 }, sends = {} }
  end
  function M.add_track(name) local t = track(name); P.tracks[#P.tracks + 1] = t; return t end
  r._add_track = M.add_track

  -- FX addressing: top level i, input FX 0x1000000 + i, container children by the documented formula.
  local function child_addr(tr, c, k) return CONTAINER + k * (#tr.fx + 1) + (c + 1) end
  local function resolve(tr, a)
    if a >= CONTAINER then
      local x = a - CONTAINER
      local n1 = #tr.fx + 1
      local c, k = x % n1 - 1, x // n1
      local parent = tr.fx[c + 1]
      return parent and parent.children and parent.children[k]
    elseif a >= INPUT then
      return tr.recfx[a - INPUT + 1]
    end
    return tr.fx[a + 1]
  end
  r._resolve = resolve

  local function log(name, ...) P.calls[#P.calls + 1] = { name, ... } end

  -- project and device
  function r.GetAppVersion() return '7.77/win64' end
  function r.GetOS() return 'Win64' end
  function r.EnumProjects() return P end
  function r.GetProjectName() return o.project_name or 'Fixture.rpp' end
  function r.GetResourcePath() return __tmpdir end
  function r.GetAudioDeviceInfo(k)
    local d = o.device or { MODE = 'ASIO', SRATE = '48000', BSIZE = '256' }
    if d[k] then return true, d[k] end
    return false, ''
  end
  function r.GetInputOutputLatency() return 300, 336 end
  function r.GetSetProjectInfo(_, k, v, set)
    if set then P.info[k] = v end
    return P.info[k] or 0
  end
  function r.RecursiveCreateDirectory(path) __mkdir(path) return 1 end
  function r.UpdateTimeline() end

  -- tempo map: 4/4 at P.bpm throughout (enough for the modules' arithmetic)
  local function spq() return 60 / P.bpm end
  function r.SetCurrentBPM(_, bpm) P.bpm = bpm end
  function r.SetTempoTimeSigMarker(_, idx, t, measure, beat, bpm, num, den, linear)
    log('SetTempoTimeSigMarker', measure, bpm, num, den)
    P.tempo[#P.tempo + 1] = { measure = measure, bpm = bpm, num = num, den = den }
    return true
  end
  function r.TimeMap_GetMeasureInfo(_, m) return 0, m * 4, (m + 1) * 4, 4, 4, P.bpm end
  function r.TimeMap2_QNToTime(_, qn) return qn * spq() end
  function r.TimeMap2_timeToQN(_, t) return t / spq() end
  function r.TimeMap2_timeToBeats(_, t)
    local qn = t / spq()
    local m = math.floor(qn / 4)
    return qn - 4 * m, m, 4, qn, 4
  end
  function r.TimeMap_GetTimeSigAtTime() return 4, 4, P.bpm end

  -- tracks
  function r.CountTracks() return #P.tracks end
  function r.GetTrack(_, i) return P.tracks[i + 1] end
  function r.GetMasterTrack() return P.master end
  function r.InsertTrackAtIndex(i) table.insert(P.tracks, i + 1, track('')) end
  function r.GetTrackName(tr) return true, tr.name end
  function r.GetTrackGUID(tr) return tr.guid end
  function r.GetSetMediaTrackInfo_String(tr, k, v, set)
    if k == 'P_NAME' and set then tr.name = v; tr.guid = '{' .. v .. '}' end
    return true, tr.name
  end
  function r.GetMediaTrackInfo_Value(tr, k)
    if k == 'IP_TRACKNUMBER' then
      if tr == P.master then return -1 end
      for i, t in ipairs(P.tracks) do if t == tr then return i end end
      return 0
    end
    return tr.info[k] or 0
  end
  function r.SetMediaTrackInfo_Value(tr, k, v) tr.info[k] = v return true end
  function r.GetTrackNumSends(tr) return #tr.sends end
  function r.GetTrackSendInfo_Value(tr, _, i) return tr.sends[i + 1] end
  function r.CreateTrackSend(src, dest) src.sends[#src.sends + 1] = dest return #src.sends - 1 end

  -- FX
  function r.TrackFX_GetCount(tr) return #tr.fx end
  function r.TrackFX_GetRecCount(tr) return #tr.recfx end
  function r.TrackFX_GetFXName(tr, a) local f = resolve(tr, a) return f ~= nil, f and f.name or '' end
  function r.TrackFX_GetFXGUID(tr, a) return resolve(tr, a).guid end
  function r.TrackFX_GetEnabled(tr, a) return resolve(tr, a).enabled end
  function r.TrackFX_GetOffline(tr, a) return resolve(tr, a).offline end
  function r.TrackFX_SetEnabled(tr, a, v) resolve(tr, a).enabled = v end
  function r.TrackFX_SetOffline(tr, a, v) resolve(tr, a).offline = v end
  function r.TrackFX_SetParam(tr, a, i, v) resolve(tr, a).params[i] = v return true end
  function r.TrackFX_GetParam(tr, a, i) return resolve(tr, a).params[i] or 0, 0, 1 end
  function r.TrackFX_Delete(tr, a) table.remove(tr.fx, a + 1) return true end
  function r.TrackFX_GetNamedConfigParm(tr, a, key)
    local f = resolve(tr, a)
    if not f then return false, '' end
    if key == 'pdc' then if f.pdc then return true, tostring(f.pdc) end return false, '' end
    if key == 'fx_type' then return true, f.type end
    if key == 'fx_ident' then if f.ident then return true, f.ident end return false, '' end
    if key == 'is_instrument' then if f.instrument ~= nil then return true, f.instrument and '1' or '0' end return false, '' end
    if key == 'container_count' then if f.children then return true, tostring(#f.children) end return false, '' end
    local k = key:match('^container_item%.(%d+)$')
    if k and f.children and a < INPUT then
      k = tonumber(k)
      if f.children[k + 1] then return true, tostring(child_addr(tr, a, k + 1)) end
    end
    return false, ''
  end
  function r.TrackFX_AddByName(tr, name, rec, inst)
    if P.installed and not P.installed[name] then return -1 end
    local list = rec and tr.recfx or tr.fx
    local item = fx(name:gsub('^JS:%s*', ''), { type = name:match('^JS:') and 'JS' or 'VST' })
    if name == 'Container' then item.children = {} item.type = 'Container' end
    list[#list + 1] = item
    return #list - 1
  end
  function r.TrackFX_CopyToTrack(src, s, dest, d, move)
    assert(src == dest and move, 'fake only moves within a track')
    local item = src.fx[s + 1]
    local x = d - CONTAINER
    local n1 = #src.fx + 1
    local c, k = x % n1 - 1, x // n1
    table.remove(src.fx, s + 1)
    local parent = src.fx[c + 1]
    table.insert(parent.children, k, item)
  end

  -- items and MIDI
  function r.AddMediaItemToTrack(tr) local it = { track = tr, info = {} } P.items[#P.items + 1] = it return it end
  function r.AddTakeToMediaItem(it) it.take = { item = it, notes = {} } return it.take end
  function r.PCM_Source_CreateFromFile(path) return { path = path } end
  function r.SetMediaItemTake_Source(take, src) take.source = src return true end
  function r.SetMediaItemInfo_Value(it, k, v) it.info[k] = v return true end
  function r.CreateNewMIDIItemInProj(tr, a, b, qn)
    local it = r.AddMediaItemToTrack(tr)
    it.midi = { a, b, qn }
    r.AddTakeToMediaItem(it)
    return it
  end
  function r.GetActiveTake(it) return it.take end
  function r.MIDI_GetPPQPosFromProjQN(_, qn) return qn * 960 end
  function r.MIDI_InsertNote(take, sel, mute, p0, p1, ch, pitch, vel) take.notes[#take.notes + 1] = { p0, p1, pitch } return true end
  function r.MIDI_Sort(take) take.sorted = true end

  -- markers, loop, transport
  function r.AddProjectMarker2(_, isrgn, a, b, name) P.markers[#P.markers + 1] = { isrgn, a, b, name } return #P.markers - 1 end
  function r.CountProjectMarkers()
    local m, g = 0, 0
    for _, x in ipairs(P.markers) do if x[1] then g = g + 1 else m = m + 1 end end
    return #P.markers, m, g
  end
  function r.EnumProjectMarkers3(_, i)
    local x = P.markers[i + 1]
    return i + 1, x[1], x[2], x[3], x[4], i + 1, 0
  end
  function r.GetSet_LoopTimeRange2(_, set, _, a, b)
    if set then P.loop = { a, b } end
    return P.loop[1], P.loop[2]
  end
  function r.GetSetRepeatEx(_, v)
    if v >= 0 then P.repeat_on = v end
    return P.repeat_on
  end
  function r.GetProjectStateChangeCount() return P.state_count or 1 end
  function r.GetPlayStateEx() return P.play_state end
  function r.GetPlayPositionEx() return P.pos end
  function r.GetPlayPosition2Ex() return P.pos2 end
  function r.GetCursorPositionEx() return P.cursor end
  function r.Master_GetPlayRate() return P.rate end

  -- clock, xruns, shared memory and the script runtime
  P.now = 1000
  P.gmem = {}
  P.console = {}
  function r.time_precise() return P.now end
  function r.GetUnderrunTime() return P.xrun_audio or 0, P.xrun_media or 0, math.floor(P.now * 1000) end
  function r.gmem_attach(name) P.gmem_name = name end
  function r.gmem_read(i) return P.gmem[i] or 0 end
  function r.defer(fn) P.deferred = fn end
  function r.atexit(fn) P.atexit = fn end
  function r.set_action_options(f) P.action_options = f end
  function r.get_action_context() return false, P.script_path or '', 0, 0, 0, 0, 0, '' end
  function r.ShowConsoleMsg(s) P.console[#P.console + 1] = s end
  P.ext = {}
  function r.GetExtState(section, key) return P.ext[section .. '/' .. key] or '' end
  function r.SetExtState(section, key, value) P.ext[section .. '/' .. key] = value end

  return r
end

return M
