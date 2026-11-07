-- @description SessionScope: record trace (transport + sensor)
-- @version 0.1.0
-- @author TRIVUNE
-- @about
--   Records what the SessionScope analyser needs, until you run the action again:
--     sensor.csv     one row per audio block from the SessionScope Sensor JSFX on the master track,
--                    with REAPER's own bar/beat for that block's position (spikes S1, S2)
--     transport.csv  REAPER's transport, polled about 30 times a second (spike B)
--     events.csv     starts, stops, seeks, loop wraps, tempo changes, REAPER xruns, dropped records
--     meta.json      host, audio device and project facts, plus a summary when recording stops
--   Output: <REAPER resource path>/SessionScope/traces/<date-time>/
--   Analyse with: build/engine/sessionscope.exe report <that folder>

local dir = ({ reaper.get_action_context() })[2]:match('^(.*[/\\])')
package.path = dir .. 'lib/?.lua;' .. package.path

local recorder = require('recorder')
local util = require('util')

local r = reaper
local function log(msg) r.ShowConsoleMsg('SessionScope: ' .. msg .. '\n') end

local out = util.join(util.join(util.join(r.GetResourcePath(), 'SessionScope'), 'traces'), util.stamp())
local rec = recorder.start(r, r.EnumProjects(-1), out, log)

local function loop()
  rec.tick()
  r.defer(loop)
end

r.atexit(function()
  local s = rec.finish()
  if r.set_action_options then r.set_action_options(8) end
  log(string.format('trace stopped after %.0f s: %d sensor records, %d dropped, %d REAPER xruns.\n  %s',
    rec.meta.duration_s, s.sensor_records, s.dropped_records, s.audio_xruns, out))
end)
if r.set_action_options then r.set_action_options(1 | 4) end   -- run again to stop; toolbar toggle shows it
log('recording a trace. Run this action again to stop.\n  ' .. out)
r.defer(loop)
