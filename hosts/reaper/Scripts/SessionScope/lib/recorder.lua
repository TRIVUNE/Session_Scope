-- The trace recorder: transport (spike B), the sensor ring (S1, S2), REAPER's xruns and the
-- Workload ground truth, written as CSV with a meta.json (format: docs/decisions/0004).
--
-- The action script and the headless validation both drive it the same way:
--   local rec = recorder.start(reaper, project, folder)   -- once
--   rec.tick()                                            -- from a defer loop, ~30 times a second
--   rec.finish()                                          -- once, when recording stops
--
-- Everything here runs on REAPER's main thread, never on the audio thread. Rows are buffered and
-- written to disk once a second, so a tick costs little more than the API calls it makes.

local json = require('json')
local ring = require('ring')
local transport = require('transport')
local util = require('util')
local workload = require('workload')

local M = {}

M.VERSION = '0.1.0'
M.EXT_SECTION = 'SessionScope'   -- ExtState section shared with the native extension

-- Column names, in file order. The engine's trace reader (engine/src/trace.h) reads the same names;
-- engine/tests/test_recorder_columns.cpp fails if the two lists drift apart.
M.SENSOR_COLUMNS = {
  'seq', 'instance', 't', 'frames', 'srate', 'play_state', 'play_pos', 'qn', 'tempo', 'ts_num', 'ts_den', 'self_us',
  'r_measure', 'r_beat', 'r_qn', 'workload_us',
}
M.TRANSPORT_COLUMNS = {
  't', 'pass', 'play_state', 'pos', 'pos2', 'cursor', 'rate', 'repeat', 'loop_start', 'loop_end',
  'tempo', 'ts_num', 'ts_den', 'measure', 'beat', 'state_count',
}
M.EVENT_COLUMNS = { 't', 'kind', 'detail' }

-- The audio device REAPER has open: driver mode, sample rate, block size and I/O latency.
local function device(r)
  local d = {}
  for _, k in ipairs({ 'MODE', 'SRATE', 'BSIZE' }) do
    local ok, v = r.GetAudioDeviceInfo(k)
    if ok then d[k:lower()] = tonumber(v) or v end
  end
  d.input_latency_samples, d.output_latency_samples = r.GetInputOutputLatency()
  return d
end

-- Starts recording proj into the folder out (created if needed). log(msg) receives notices.
function M.start(r, proj, out, log)
  log = log or function() end
  r.RecursiveCreateDirectory(out, 0)
  local sensor_csv = util.csv(util.join(out, 'sensor.csv'), M.SENSOR_COLUMNS)
  local transport_csv = util.csv(util.join(out, 'transport.csv'), M.TRANSPORT_COLUMNS)
  local events_csv = util.csv(util.join(out, 'events.csv'), M.EVENT_COLUMNS)

  local t_start = r.time_precise()
  local ext = r.GetExtState(M.EXT_SECTION, 'host_ext')
  local meta = {
    schema = 'sessionscope.trace',
    schema_version = 1,
    recorder_version = M.VERSION,
    started_utc = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    t_start = t_start,
    clock = 'time_precise (seconds); sensor and recorder share it',
    host = { name = 'REAPER', version = r.GetAppVersion(), os = r.GetOS() },
    device = device(r),
    project = { name = r.GetProjectName(proj) },
    ring = { segment = ring.SEGMENT, layout = ring.LAYOUT },
    -- The SessionScope REAPER extension (hosts/reaper/extension), when installed, times REAPER's
    -- own audio blocks into host.csv in this folder while the recorder runs (decision 0003).
    host_extension = ext ~= '' and ext or nil,
  }
  -- Tells the extension where to write host.csv. Cleared again in finish().
  r.SetExtState(M.EXT_SECTION, 'host_trace_dir', out, false)
  local function write_meta() util.write_file(util.join(out, 'meta.json'), json.encode(meta, '  ') .. '\n') end
  write_meta()

  r.gmem_attach(ring.SEGMENT)            -- the shared memory the sensor JSFX writes into
  local rd = ring.new(r.gmem_read)
  local tp = {}                          -- transport event detector state (lib/transport.lua)
  local counts = { sensor = 0, transport = 0, xruns_audio = 0, xruns_media = 0 }
  local sensor_seen, second_warned, last_dropped, last_resets = false, false, 0, 0
  local both_alive_since   -- when two sensor instances were first seen writing at the same time
  local xr_audio, xr_media -- REAPER's last xrun timestamps, to notice new ones
  local wl, wl_state = {}, nil           -- Workload JSFX instances, rescanned when the project changes
  local last_flush = t_start

  local function event(t, kind, detail) events_csv.row({ t, kind, detail or '' }) end

  -- One transport sample: play state, positions, loop, tempo and bar, plus any events it reveals.
  local function sample_transport(now)
    local state = r.GetPlayStateEx(proj)
    local pos2 = r.GetPlayPosition2Ex(proj)          -- the next block REAPER will process
    local ls, le = r.GetSet_LoopTimeRange2(proj, false, true, 0, 0, false)
    local num, den, tempo = r.TimeMap_GetTimeSigAtTime(proj, pos2)
    local beat, measure = r.TimeMap2_timeToBeats(proj, pos2)
    local s = {
      t = now, state = state, pos = r.GetPlayPositionEx(proj), pos2 = pos2, cursor = r.GetCursorPositionEx(proj),
      rate = r.Master_GetPlayRate(proj), repeat_on = r.GetSetRepeatEx(proj, -1) == 1, loop_start = ls, loop_end = le,
      tempo = tempo, ts_num = num, ts_den = den,
    }
    local evs, pass = transport.step(tp, s)
    for _, e in ipairs(evs) do event(e.t, e.kind, e.detail) end
    transport_csv.row({ now, pass, state, s.pos, pos2, s.cursor, s.rate, s.repeat_on, ls, le, tempo, num, den,
      measure, beat, r.GetProjectStateChangeCount(proj) })
    counts.transport = counts.transport + 1
  end

  -- REAPER keeps the time of its last audio and media underrun; a changed value is a new xrun.
  -- The timestamps are in REAPER's millisecond clock, converted to time_precise() seconds here.
  local function check_xruns(now)
    local ax, mx, cur = r.GetUnderrunTime()
    if xr_audio and ax ~= xr_audio and ax > 0 then
      counts.xruns_audio = counts.xruns_audio + 1
      event(now - (cur - ax) / 1000, 'audio_xrun', 'REAPER reported an audio underrun')
    end
    if xr_media and mx ~= xr_media and mx > 0 then
      counts.xruns_media = counts.xruns_media + 1
      event(now - (cur - mx) / 1000, 'media_xrun', 'REAPER reported a media underrun')
    end
    xr_audio, xr_media = ax, mx                      -- the first call only remembers, never reports
  end

  -- Copy new sensor records out of the ring, add REAPER's bar for each, and write them.
  local function drain_sensor(now)
    local recs, hd = ring.drain(rd, 50000)
    if hd and not sensor_seen then
      sensor_seen = true
      meta.sensor = { version = hd.version, capacity = hd.cap, instance = hd.owner }
      event(now, 'sensor', string.format('sensor v%s connected', tostring(hd.version)))
    end
    -- Two sensors only count as running when both keep writing for over a second (test T10); a new
    -- instance waiting to take over from a closed project's sensor is a handover, not a second sensor.
    if hd and hd.other ~= hd.owner and now - hd.other_t < 0.5 and now - hd.owner_t < 0.5 then
      both_alive_since = both_alive_since or now
      if not second_warned and now - both_alive_since > 1 then
        second_warned = true
        event(now, 'second_sensor', 'more than one SessionScope Sensor is running; only one is recorded')
        log('more than one SessionScope Sensor is running. Keep one, on the master track.')
      end
    else
      both_alive_since = nil
    end
    if rd.dropped ~= last_dropped then
      event(now, 'dropped', string.format('%d records lost in total (recorder fell behind)', rd.dropped))
      last_dropped = rd.dropped
    end
    if rd.resets ~= last_resets then
      event(now, 'ring_reset', 'the sensor restarted its ring')
      last_resets = rd.resets
    end
    if #recs == 0 then return end
    -- Rescan Workload instances only when the project changed, not on every tick.
    local sc = r.GetProjectStateChangeCount(proj)
    if sc ~= wl_state then wl, wl_state = workload.scan(r, proj), sc end
    for _, x in ipairs(recs) do
      local beat, measure = r.TimeMap2_timeToBeats(proj, x.play_pos)   -- REAPER's own bar for this block
      sensor_csv.row({ x.seq, x.instance, x.t, x.frames, x.srate, x.play_state, x.play_pos, x.qn, x.tempo, x.ts_num, x.ts_den,
        x.self_us, measure, beat, r.TimeMap2_timeToQN(proj, x.play_pos), #wl > 0 and workload.total(wl, x.qn, x.frames) or nil })
    end
    counts.sensor = counts.sensor + #recs
  end

  local rec = { out = out, counts = counts, meta = meta }

  function rec.tick()
    local now = r.time_precise()
    sample_transport(now)
    check_xruns(now)
    drain_sensor(now)
    if now - last_flush >= 1 then          -- write to disk once a second, not every tick
      sensor_csv.flush()
      transport_csv.flush()
      events_csv.flush()
      last_flush = now
    end
  end

  function rec.finish()
    r.SetExtState(M.EXT_SECTION, 'host_trace_dir', '', false)   -- the extension closes host.csv
    local now = r.time_precise()
    event(now, 'recorder_stop', '')
    meta.stopped_utc = os.date('!%Y-%m-%dT%H:%M:%SZ')
    meta.duration_s = now - t_start
    meta.summary = {
      sensor_records = counts.sensor, transport_samples = counts.transport, passes = tp.pass or 0,
      dropped_records = rd.dropped, ring_resets = rd.resets, audio_xruns = counts.xruns_audio, media_xruns = counts.xruns_media,
      sensor_connected = sensor_seen, second_sensor = second_warned,
    }
    write_meta()
    sensor_csv.close()
    transport_csv.close()
    events_csv.close()
    return meta.summary
  end

  event(t_start, 'recorder_start', 'recorder ' .. M.VERSION)
  ring.drain(rd)   -- skip what the sensor wrote before recording started
  rec.tick()
  return rec
end

return M
