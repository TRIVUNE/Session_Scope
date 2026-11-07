local workload = require('workload')
local fake = require('fake_reaper')

local function p(t)
  return { cost_us = t[1] or 0, mode = t[2] or 0, from_qn = t[3] or 0, to_qn = t[4] or 0, step_us = t[5] or 0, step_qn = t[6] or 8 }
end

test('fixed cost inside the window, nothing outside', function()
  local w = p({ 1500, 0, 128, 192 })
  eq(workload.cost(w, 127.9, 256), 0)
  eq(workload.cost(w, 128, 256), 1500)
  eq(workload.cost(w, 191.9, 512), 1500)
  eq(workload.cost(w, 192, 256), 0)
end)

test('proportional mode scales with block length', function()
  eq(workload.cost(p({ 500, 1 }), 0, 512), 1000)
  eq(workload.cost(p({ 500, 1 }), 0, 128), 250)
end)

test('staircase: F7 reaches 5500 us per 256 samples in bars 23-24', function()
  local w = p({ 0, 1, 0, 0, 500, 8 })
  eq(workload.cost(w, 0, 256), 0)
  eq(workload.cost(w, 8, 256), 500)
  eq(workload.cost(w, 22 * 4 + 1, 256), 5500)
end)

test('scan finds enabled Workload instances and reads their sliders', function()
  local r = fake.new()
  local t = r._add_track('Load')
  t.fx[1] = fake.fx('JS: SessionScope Workload (test load, fixture F7)', { params = { [0] = 700, 0, 0, 0, 0, 8 } })
  t.fx[2] = fake.fx('JS: SessionScope Workload (test load, fixture F7)', { params = { [0] = 900 }, enabled = false })
  t.fx[3] = fake.fx('ReaEQ')
  local list = workload.scan(r, r.EnumProjects(-1))
  eq(#list, 1)
  eq(workload.total(list, 4, 256), 700)
end)

test('the slider order matches the JSFX', function()
  local src = read_file(__repo .. '/hosts/reaper/Effects/SessionScope/SessionScope Workload.jsfx')
  local names = {}
  for i, name in src:gmatch('slider(%d+):([%w_]+)=') do names[tonumber(i)] = name end
  eq(table.concat(names, ','), 'cost_us,mode,from_qn,to_qn,step_us,step_qn')
end)
