local inventory = require('inventory')
local json = require('json')
local fake = require('fake_reaper')

local function project()
  local r = fake.new()
  local vox = r._add_track('Lead Vox')
  vox.info.I_RECARM = 1
  vox.info.I_PERFFLAGS = 2
  vox.recfx[1] = fake.fx('JS: Fixed Latency', { pdc = 64, guid = '{in}' })
  vox.fx = {
    fake.fx('ReaEQ', { pdc = 0, guid = '{eq1}', ident = 'C:\\Users\\someone\\VST\\reaeq.dll' }),
    fake.fx('JS: Fixed Latency', { pdc = 1024, guid = '{lat}' }),
    fake.fx('ReaEQ', { pdc = 0, guid = '{eq2}' }),
    fake.fx('JS: Fixed Latency', { pdc = 512, enabled = false }),
    fake.fx('Container', { pdc = 256, children = { fake.fx('JS: Fixed Latency', { pdc = 256, guid = '{inner}' }), fake.fx('ReaEQ', { pdc = 0 }) } }),
  }
  local synth = r._add_track('Synth')
  synth.fx = { fake.fx('Mystery Synth', { instrument = true }) }   -- PDC read fails for this one
  r._p.master.fx = { fake.fx('JS: SessionScope Sensor v0 (REAPER stand-in)', { pdc = 0 }) }
  r.AddProjectMarker2(nil, true, 64, 96, 'Chorus')
  return r
end

test('tracks, FX, input FX and container contents are listed', function()
  local inv = inventory.collect(project(), nil)
  eq(inv.project.track_count, 2)
  eq(inv.project.fx_count, 10)
  local vox = inv.tracks[1]
  eq(vox.name, 'Lead Vox')
  eq(vox.rec_arm, true)
  eq(vox.no_anticipative_fx, true)
  eq(#vox.fx, 5)
  eq(#vox.input_fx, 1)
  eq(vox.input_fx[1].address, 0x1000000)
  eq(#vox.fx[5].container, 2)
  eq(vox.fx[5].container[1].guid, '{inner}')
  eq(inv.master.monitoring_fx ~= nil, true)
end)

test('duplicate FX names keep distinct identities (T01)', function()
  local vox = inventory.collect(project(), nil).tracks[1]
  eq(vox.fx[1].name, vox.fx[3].name)
  truthy(vox.fx[1].guid ~= vox.fx[3].guid, 'distinct GUIDs')
end)

test('PDC in samples and milliseconds at the device rate (T02)', function()
  local vox = inventory.collect(project(), nil).tracks[1]
  eq(vox.fx[2].pdc_samples, 1024)
  near(vox.fx[2].pdc_ms, 21.3333333, 1e-6)
  local r = project()
  r = fake.new({ device = { SRATE = '96000', BSIZE = '128' } })
  local t = r._add_track('x')
  t.fx = { fake.fx('JS: Fixed Latency', { pdc = 1024 }) }
  near(inventory.collect(r, nil).tracks[1].fx[1].pdc_ms, 10.6666667, 1e-6)
end)

test('an unreadable PDC stays unknown, not zero', function()
  local inv = inventory.collect(project(), nil)
  local synth = inv.tracks[2]
  eq(synth.fx[1].pdc_samples, nil)
  eq(synth.chain.complete, false)
  eq(synth.fx[1].is_instrument, true)
end)

test('chain model: enabled FX only, containers mark it incomplete', function()
  local vox = inventory.collect(project(), nil).tracks[1]
  eq(vox.chain.pdc_samples, 0 + 1024 + 0 + 256)
  eq(vox.chain.has_containers, true)
  eq(vox.chain.complete, false)
end)

test('private paths are reduced to file names', function()
  eq(inventory.collect(project(), nil).tracks[1].fx[1].ident, 'reaeq.dll')
end)

test('capabilities come from what could be read', function()
  local caps = inventory.collect(project(), nil).capabilities
  eq(caps['metric.fx_pdc'], true)
  eq(caps['metric.block_timing'], true)
  eq(caps['metric.host_rt'], false)
  local empty = fake.new()
  local c2 = inventory.collect(empty, nil).capabilities
  eq(c2['metric.fx_pdc'], false)
  eq(c2['metric.block_timing'], false)
end)

test('regions become named sections with measures', function()
  local inv = inventory.collect(project(), nil)
  eq(#inv.project.regions, 1)
  eq(inv.project.regions[1].name, 'Chorus')
  eq(inv.project.regions[1].start_measure, 32)
end)

test('the inventory encodes to JSON and the summary is readable', function()
  local inv = inventory.collect(project(), nil)
  local text = json.encode(inv, '  ')
  truthy(text:find('"schema": "sessionscope.inventory"', 1, true), 'schema in JSON')
  local s = inventory.summary(inv)
  truthy(s:find('2 tracks, 10 FX', 1, true), 'counts in summary: ' .. s)
  truthy(s:find('Lead Vox', 1, true), 'track in summary')
  truthy(s:find('(incomplete model)', 1, true), 'incomplete marked')
end)
