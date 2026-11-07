-- Project inventory (spike A) and declared plugin delay (spike C).
-- Every value comes from a documented ReaScript call; a call that fails leaves the field empty
-- rather than zero. `r` is the reaper API table, passed in so tests can use a fake host.

local json = require('json')
local util = require('util')

local M = {}

M.SCHEMA = 'sessionscope.inventory'
M.SCHEMA_VERSION = 1
M.INPUT_FX = 0x1000000

-- One TrackFX_GetNamedConfigParm value (pdc, fx_type, container_count, ...), or nil if REAPER
-- does not have it for this FX.
local function named(r, track, fx, key)
  local ok, v = r.TrackFX_GetNamedConfigParm(track, fx, key)
  if ok then return v end
  return nil
end

-- One FX at address addr (a chain index, input-FX index or container address), with its
-- container contents, recursively. stats counts FX and successful PDC reads for the capabilities.
function M.fx(r, track, addr, srate, stats)
  local _, name = r.TrackFX_GetFXName(track, addr)
  local pdc = tonumber(named(r, track, addr, 'pdc'))
  stats.fx = stats.fx + 1
  if pdc then stats.pdc_ok = stats.pdc_ok + 1 end
  local e = {
    address = addr,
    guid = r.TrackFX_GetFXGUID(track, addr),
    name = name,
    type = named(r, track, addr, 'fx_type'),
    ident = util.basename(named(r, track, addr, 'fx_ident')),   -- file name only, never a private path
    enabled = r.TrackFX_GetEnabled(track, addr),
    offline = r.TrackFX_GetOffline(track, addr),
    pdc_samples = pdc,
    pdc_ms = (pdc and srate) and 1000 * pdc / srate or nil,
  }
  local inst = named(r, track, addr, 'is_instrument')       -- REAPER 7.40+
  if inst then e.is_instrument = inst == '1' end
  local count = tonumber(named(r, track, addr, 'container_count'))
  if count then
    e.container = json.array()
    for i = 0, count - 1 do
      local child = tonumber(named(r, track, addr, 'container_item.' .. i))
      if child then e.container[#e.container + 1] = M.fx(r, track, child, srate, stats) end
    end
  end
  return e
end

-- Declared delay of a chain as a serial sum. This is a model, labelled as one: containers may run
-- their contents in parallel, and REAPER's own compensation can differ (blueprint 6.2).
function M.chain(list)
  local sum, complete, containers = 0, true, false
  for _, e in ipairs(list) do
    if e.container then containers = true end
    if e.enabled and not e.offline then
      if e.pdc_samples then sum = sum + e.pdc_samples else complete = false end
    end
  end
  return { model = 'serial sum of enabled FX', pdc_samples = sum, complete = complete and not containers, has_containers = containers }
end

-- The track's sends, identified by their destination track's GUID.
local function sends(r, tr)
  local out = json.array()
  for i = 0, r.GetTrackNumSends(tr, 0) - 1 do
    local ok, dest = pcall(r.GetTrackSendInfo_Value, tr, 0, i, 'P_DESTTRACK')
    out[#out + 1] = { index = i, dest_guid = (ok and type(dest) ~= 'number') and r.GetTrackGUID(dest) or nil }
  end
  return out
end

-- One track (or the master): identity, recording and performance flags, FX chains, chain delay.
function M.track(r, tr, srate, stats, is_master)
  local _, name = r.GetTrackName(tr)
  local perf = r.GetMediaTrackInfo_Value(tr, 'I_PERFFLAGS')
  local e = {
    number = r.GetMediaTrackInfo_Value(tr, 'IP_TRACKNUMBER'),
    guid = r.GetTrackGUID(tr),
    name = name,
    fx_enabled = r.GetMediaTrackInfo_Value(tr, 'I_FXEN') ~= 0,
    no_anticipative_fx = math.floor(perf) & 2 == 2,
    fx = json.array(),
  }
  if not is_master then
    e.folder_depth = r.GetMediaTrackInfo_Value(tr, 'I_FOLDERDEPTH')
    e.rec_arm = r.GetMediaTrackInfo_Value(tr, 'I_RECARM') == 1
    e.rec_mon = r.GetMediaTrackInfo_Value(tr, 'I_RECMON')
    e.rec_input = r.GetMediaTrackInfo_Value(tr, 'I_RECINPUT')
    e.sends = sends(r, tr)
  end
  for i = 0, r.TrackFX_GetCount(tr) - 1 do e.fx[#e.fx + 1] = M.fx(r, tr, i, srate, stats) end
  -- Record-input FX on tracks; on the master, REAPER keeps monitoring FX in this chain.
  local extra = json.array()
  for i = 0, r.TrackFX_GetRecCount(tr) - 1 do extra[#extra + 1] = M.fx(r, tr, M.INPUT_FX + i, srate, stats) end
  e[is_master and 'monitoring_fx' or 'input_fx'] = extra
  e.chain = M.chain(e.fx)
  return e
end

local function device(r)
  local d = {}
  for _, k in ipairs({ 'MODE', 'SRATE', 'BSIZE' }) do
    local ok, v = r.GetAudioDeviceInfo(k)
    if ok then d[k:lower()] = tonumber(v) or v end
  end
  local inl, outl = r.GetInputOutputLatency()
  d.input_latency_samples, d.output_latency_samples = inl, outl
  if d.srate and d.bsize then d.block_ms = 1000 * d.bsize / d.srate end
  return d
end

-- The project's regions with their start and end measures, as part of the inventory data.
-- (Reports refer to bars by number only; region names are not shown.)
function M.regions(r, proj)
  local out = json.array()
  local _, markers, regions = r.CountProjectMarkers(proj)
  for i = 0, markers + regions - 1 do
    local _, isrgn, pos, rgnend, name = r.EnumProjectMarkers3(proj, i)
    if isrgn then
      local _, m0 = r.TimeMap2_timeToBeats(proj, pos)
      local _, m1 = r.TimeMap2_timeToBeats(proj, rgnend)
      out[#out + 1] = { name = name, start = pos, ['end'] = rgnend, start_measure = m0, end_measure = m1 }
    end
  end
  return out
end

-- Is a SessionScope Sensor in this FX list (the master chain)?
local function has_sensor(list)
  for _, e in ipairs(list) do
    if e.name and e.name:find('SessionScope Sensor', 1, true) then return true end
  end
  return false
end

-- The whole inventory of proj as one table, ready for json.encode.
-- Delays are converted to milliseconds at the audio device's rate (the project's if no device is open).
function M.collect(r, proj)
  local dev = device(r)
  local proj_srate = r.GetSetProjectInfo(proj, 'PROJECT_SRATE', 0, false)
  local use_proj = r.GetSetProjectInfo(proj, 'PROJECT_SRATE_USE', 0, false) == 1
  local srate = dev.srate or (use_proj and proj_srate) or nil
  local stats = { fx = 0, pdc_ok = 0 }
  local master = M.track(r, r.GetMasterTrack(proj), srate, stats, true)
  local tracks = json.array()
  for i = 0, r.CountTracks(proj) - 1 do tracks[#tracks + 1] = M.track(r, r.GetTrack(proj, i), srate, stats, false) end
  return {
    schema = M.SCHEMA,
    schema_version = M.SCHEMA_VERSION,
    created_utc = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    host = { name = 'REAPER', version = r.GetAppVersion(), os = r.GetOS() },
    device = dev,
    project = {
      name = r.GetProjectName(proj),
      srate = use_proj and proj_srate or nil,
      ms_conversion_srate = srate,
      track_count = #tracks,
      fx_count = stats.fx,
      regions = M.regions(r, proj),
    },
    -- Capability flags come from what this run could actually read (blueprint 9.2), never from the DAW name.
    capabilities = {
      ['transport.musical'] = true,
      ['project.inventory'] = true,
      ['metric.fx_pdc'] = stats.pdc_ok > 0,
      ['metric.block_timing'] = has_sensor(master.fx),
      ['metric.host_rt'] = false,
      ['metric.track_time'] = false,
      ['metric.fx_time'] = false,
    },
    notes = json.array({
      'metric.host_rt: no ReaScript call returns RT CPU; candidate is the extension audio hook (docs/research/spike-D.md).',
      'chain.pdc_samples is a modelled serial sum, not REAPER\'s compensated latency.',
    }),
    master = master,
    tracks = tracks,
  }
end

-- A few lines for the ReaScript console.
function M.summary(inv)
  local lines = {}
  local d = inv.device
  lines[#lines + 1] = string.format('SessionScope inventory: %s, %d tracks, %d FX (REAPER %s)',
    inv.project.name ~= '' and inv.project.name or '(unsaved project)', inv.project.track_count, inv.project.fx_count, inv.host.version)
  lines[#lines + 1] = string.format('Device: %s Hz, %s samples per block%s', tostring(d.srate or '?'), tostring(d.bsize or '?'),
    d.block_ms and string.format(' (%.2f ms deadline)', d.block_ms) or '')
  local function line(t)
    local c = t.chain
    if c.pdc_samples > 0 or not c.complete then
      lines[#lines + 1] = string.format('  %-24s %5d samples%s%s', t.name, c.pdc_samples,
        c.pdc_samples > 0 and inv.project.ms_conversion_srate and string.format(' = %.2f ms', 1000 * c.pdc_samples / inv.project.ms_conversion_srate) or '',
        c.complete and '' or ' (incomplete model)')
    end
  end
  lines[#lines + 1] = 'Declared plugin delay per chain (modelled serial sum):'
  line(inv.master)
  for _, t in ipairs(inv.tracks) do line(t) end
  lines[#lines + 1] = 'Capabilities:'
  local keys = {}
  for k in pairs(inv.capabilities) do keys[#keys + 1] = k end
  table.sort(keys)
  for _, k in ipairs(keys) do lines[#lines + 1] = string.format('  %-20s %s', k, inv.capabilities[k] and 'yes' or 'no') end
  return table.concat(lines, '\n') .. '\n'
end

return M
