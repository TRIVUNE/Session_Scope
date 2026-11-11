-- The action scripts themselves, run end to end against the fake host.

local fake = require('fake_reaper')
local ring = require('ring')

local SCRIPTS = __repo .. '/hosts/reaper/Scripts/SessionScope/'

local function lines(text)
  local out = {}
  for l in text:gmatch('([^\n]*)\n') do out[#out + 1] = l end
  return out
end

local function split(l)
  local out = {}
  for c in (l .. ','):gmatch('([^,]*),') do out[#out + 1] = c end
  return out
end

test('every action script compiles', function()
  for _, name in ipairs({ 'SessionScope - Record trace.lua', 'SessionScope - Inventory.lua', 'SessionScope - Build fixtures.lua',
    'SessionScope - Probe all FX.lua', 'SessionScope - Remove probes.lua', '../../tests/integration/validate.lua' }) do
    local fn, err = loadfile(SCRIPTS .. name)
    truthy(fn, name .. ': ' .. tostring(err))
  end
end)

test('Record trace: sensor rows, transport rows, events and summary', function()
  local r = fake.new()
  local P = r._p
  P.script_path = SCRIPTS .. 'SessionScope - Record trace.lua'
  r._add_track('Load').fx[1] = fake.fx('JS: SessionScope Workload (test load, fixture F7)', { params = { [0] = 1000, 0, 0, 0, 0, 8 } })

  -- A sensor that writes 256-sample blocks at 48 kHz, as the JSFX would.
  local n, t_audio = 0, P.now
  local g = P.gmem
  g[0], g[1], g[2], g[3], g[4], g[5], g[9] = ring.MAGIC, ring.LAYOUT, 0, 1024, #ring.FIELDS, 77, 0.1
  local function blocks_until(t)
    while t_audio + 256 / 48000 <= t do
      t_audio = t_audio + 256 / 48000
      local p = ring.HDR + (n % 1024) * #ring.FIELDS
      local pos = (t_audio - 1000)
      local vals = { t_audio, 256, 48000, 1, pos, pos * 2, 120, 4, 4, 1.2, n, 77 }
      for i, v in ipairs(vals) do g[p + i - 1] = v end
      n = n + 1
      g[2] = n
      g[6] = t_audio
    end
  end
  blocks_until(P.now + 0.5)       -- history before recording starts: skipped

  P.play_state = 1
  reaper = r
  dofile(P.script_path)
  local out = P.console[1]:match('\n  (.-)\n$')
  truthy(out, 'output folder in the console message')
  eq(r.GetExtState('SessionScope', 'host_trace_dir'), out, 'folder announced to the extension')

  local first_recorded = n
  for i = 1, 60 do
    P.now = P.now + 1 / 30
    P.pos2 = P.now - 1000
    P.pos = P.pos2 - 0.01
    blocks_until(P.now)
    if i == 30 then P.xrun_audio = math.floor(P.now * 1000) - 20 end
    P.deferred()
  end
  P.atexit()

  local sensor = lines(read_file(out .. '/sensor.csv'))
  eq(sensor[1], 'seq,instance,t,frames,srate,play_state,play_pos,qn,tempo,ts_num,ts_den,self_us,r_measure,r_beat,r_qn,workload_us')
  eq(#sensor - 1, n - first_recorded, 'one row per block written while recording')
  local row = split(sensor[2])
  eq(#row, 16)
  eq(row[1], tostring(first_recorded))
  eq(row[16], '1000', 'workload ground truth')

  local tr = lines(read_file(out .. '/transport.csv'))
  eq(#tr - 1, 61, 'one sample at start, then one per tick')
  eq(split(tr[2])[2], '1', 'pass 1')

  local ev = read_file(out .. '/events.csv')
  for _, kind in ipairs({ 'recorder_start', 'sensor', 'start', 'audio_xrun', 'recorder_stop' }) do
    truthy(ev:find(',' .. kind .. ',', 1, true), 'event ' .. kind)
  end

  local meta = read_file(out .. '/meta.json')
  truthy(meta:find('"sensor_records": ' .. (n - first_recorded), 1, true), 'summary record count')
  truthy(meta:find('"audio_xruns": 1', 1, true), 'summary xruns')
  eq(P.action_options, 8, 'toggle state cleared on exit')
  eq(r.GetExtState('SessionScope', 'host_trace_dir'), '', 'extension told to stop')
end)

local function second_sensor_run(owner_alive)
  local recorder = require('recorder')
  local r = fake.new()
  local g = r._p.gmem
  g[0], g[1], g[2], g[3], g[4], g[5], g[9] = ring.MAGIC, ring.LAYOUT, 0, 1024, #ring.FIELDS, 77, 0.1
  local out = __tmpdir .. '/t10-' .. tostring(owner_alive)
  __mkdir(out)
  local rec = recorder.start(r, r.EnumProjects(-1), out)
  for _ = 1, 60 do                       -- two seconds of ticks
    r._p.now = r._p.now + 1 / 30
    if owner_alive then g[6] = r._p.now end
    g[7], g[8] = 88, r._p.now           -- another instance keeps announcing itself
    rec.tick()
  end
  rec.finish()
  return read_file(out .. '/events.csv')
end

test('T10: two sensors writing at once are reported', function()
  truthy(second_sensor_run(true):find(',second_sensor,', 1, true), 'warning expected')
end)

test('a new sensor taking over from a closed project is not a second sensor', function()
  eq(second_sensor_run(false):find(',second_sensor,', 1, true), nil)
end)

test('Inventory writes JSON and prints a summary', function()
  local r = fake.new()
  local P = r._p
  P.script_path = SCRIPTS .. 'SessionScope - Inventory.lua'
  r._add_track('Vox').fx[1] = fake.fx('JS: Fixed Latency', { pdc = 1024 })
  reaper = r
  dofile(P.script_path)
  local path = P.console[1]:match('Written to (.-)\n')
  truthy(path, 'path in console')
  truthy(read_file(path):find('"pdc_samples": 1024', 1, true), 'pdc in JSON')
end)
