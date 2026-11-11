-- S2 on a real audio interface (docs/research/S2-lateness.md): records the calibration staircase
-- fixtures F7b (anticipative FX off) and F7a (on), bars 1-24 twice each, with whatever audio
-- device this REAPER has open. Nothing audible is played: the fixtures hold only the Workload JSFX.
--
-- Run it in the validation's isolated REAPER (never your own), after `sessionscope reaper
-- validate` has built the fixtures there, with the device set in build/reaper-validate/reaper.ini:
--   [audioconfig] mode=3 (ASIO in REAPER 7.83), asio_driver_name="<driver>", asio_driver=<its
--   number in REAPER's list>, asio_srate=48000, asio_srate_use=1, asio_bsize=256, asio_bsize_use=1
--   reaper.exe -cfgfile build\reaper-validate\reaper.ini -newinst -nosplash -new <this file>
-- The traces land in <resource>/SessionScope/s2-real/<S2_LABEL>/ (S2_LABEL: an environment
-- variable, e.g. asio-48k-256); `sessionscope report <trace>` then gives the S2 table. Copy them
-- away before the next validation run, which clears that folder. Put the device's own settings
-- back afterwards: REAPER changes the driver's sample rate and block size for every program.
local r = reaper
local here = debug.getinfo(1, 'S').source:match('^@(.*[/\\])')
package.path = here .. '../../Scripts/SessionScope/lib/?.lua;' .. package.path
local fixtures = require('fixtures')
local json = require('json')
local recorder = require('recorder')
local util = require('util')

local base = util.join(r.GetResourcePath(), 'SessionScope')
local label = os.getenv('S2_LABEL') or 'run'
local out_root = util.join(util.join(base, 's2-real'), label)
r.RecursiveCreateDirectory(out_root, 0)

local result = { label = label, device = {}, traces = {}, errors = json.array() }
for _, k in ipairs({ 'MODE', 'IDENT_OUT', 'SRATE', 'BSIZE' }) do
  local ok, v = r.GetAudioDeviceInfo(k)
  result.device[k:lower()] = ok and v or nil
end
local function save()
  util.write_file(util.join(out_root, 'result.json'), json.encode(result, '  ') .. '\n')
end

local SECONDS = 100          -- bars 1-24 at 120 bpm last 48 s: two passes and a little more
local jobs, current = {}, nil
for _, id in ipairs({ 'F7b', 'F7a' }) do
  local proj, rec, t0
  jobs[#jobs + 1] = function()
    if not proj then
      r.Main_OnCommand(fixtures.NEW_TAB, 0)
      r.Main_openProject(util.join(util.join(util.join(base, 'fixtures'), id), id .. '.rpp'))
      proj = r.EnumProjects(-1)
      r.SetEditCurPos(0, false, false)
      rec = recorder.start(r, proj, util.join(out_root, id))
      r.CSurf_OnPlay()
      t0 = r.time_precise()
      return false
    end
    rec.tick()
    if r.time_precise() - t0 < SECONDS then return false end
    r.CSurf_OnStop()
    rec.tick()
    local summary = rec.finish()
    result.traces[id] = { folder = rec.out, summary = summary }
    save()
    r.Main_OnCommand(fixtures.CLOSE_TAB, 0)
    return true
  end
end
jobs[#jobs + 1] = function()
  result.complete = true
  save()
  r.Main_OnCommand(40004, 0)
  return true
end

local deadline = r.time_precise() + 2 * SECONDS + 90
local function tick()
  if r.time_precise() > deadline then
    result.errors[#result.errors + 1] = 'took too long'
    save()
    r.Main_OnCommand(40004, 0)
    return
  end
  current = current or table.remove(jobs, 1)
  if not current then return end
  local ok, done = pcall(current)
  if not ok then
    result.errors[#result.errors + 1] = tostring(done)
    done = true
  end
  if done then current = nil end
  r.defer(tick)
end
save()
r.defer(tick)
