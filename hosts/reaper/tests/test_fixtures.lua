-- lib/fixtures.lua: the test projects (F1-F8) are built as specified, against the fake REAPER.

local fixtures = require('fixtures')
local fake = require('fake_reaper')
local inventory = require('inventory')

local function build(id, opts)
  local spec
  for _, f in ipairs(fixtures.list) do if f.id == id then spec = f end end
  local r = fake.new(opts)
  local problems = {}
  local folder = __tmpdir .. '/fixtures/' .. id
  __mkdir(folder)
  fixtures.build(r, r.EnumProjects(-1), spec, folder, function(m) problems[#problems + 1] = m end)
  return r, problems, spec
end

test('every fixture builds without problems on a full install', function()
  for _, f in ipairs(fixtures.list) do
    local _, problems = build(f.id)
    eq(table.concat(problems, '; '), '', f.id)
  end
end)

test('ids are unique', function()
  local seen = {}
  for _, f in ipairs(fixtures.list) do
    eq(seen[f.id], nil, f.id)
    seen[f.id] = true
  end
end)

test('F4: input FX, bypassed and offline FX, and a container with two children', function()
  local r = build('F4')
  local inv = inventory.collect(r, nil)
  local vox = inv.tracks[1]
  eq(vox.rec_arm, true)
  eq(#vox.input_fx, 1)
  eq(vox.input_fx[1].address, 0x1000000)
  eq(#vox.fx, 7)
  eq(vox.fx[5].enabled, false)
  eq(vox.fx[6].offline, true)
  eq(#vox.fx[7].container, 2)
  eq(r._p.master.fx[1].params[0], 256)
end)

test('F5: tempo markers, sends and quarter-note workload windows', function()
  local r = build('F5')
  local P = r._p
  eq(#P.tempo, 3)
  eq(P.tempo[2].measure, 16)
  eq(#P.markers, 0, 'no named regions')
  local heavy = P.tracks[5]
  eq(heavy.name, 'Load bars 33-48')
  eq(heavy.fx[1].params[2], 32 * 4, 'from QN = start of bar 33')
  eq(heavy.fx[1].params[3], 48 * 4, 'until QN = start of bar 49')
  eq(P.tracks[3].sends[1], P.tracks[6], 'pad sends to the reverb bus')
  local pad_take = P.items[3].take
  eq(#pad_take.notes, 16 * 2 + 16 * 4 + 16 * 8 + 16 * 2, 'pad chords')
end)

test('F8: one tempo marker per time-signature change, each with its signature', function()
  local r = build('F8')                                   -- build F8 into a fake REAPER project
  local P = r._p                                          -- the fake project's recorded state
  eq(#P.tempo, 4)                                         -- 4/4, 3/4, 7/8, 4/4
  eq(P.tempo[2].measure, 4)                               -- bar 5 is REAPER measure 4 (0-based)
  eq(P.tempo[2].num, 3)                                   -- 3/4 from bar 5
  eq(P.tempo[3].num, 7)                                   -- 7/8 from bar 9 ...
  eq(P.tempo[3].den, 8)                                   -- ... with an eighth-note denominator
  eq(P.repeat_on, 1)                                      -- loops the whole song
end)

test('F7b turns anticipative FX off everywhere, F7a leaves it on', function()
  local a = build('F7a')
  eq(a._p.tracks[1].info.I_PERFFLAGS, 0)
  local b = build('F7b')
  eq(b._p.tracks[1].info.I_PERFFLAGS & 2, 2)
  eq(b._p.master.info.I_PERFFLAGS & 2, 2)
  eq(b._p.repeat_on, 1)
end)

test('tone media is real WAV audio', function()
  build('F1')
  local data = read_file(__tmpdir .. '/fixtures/F1/media/tone_110.wav')
  eq(data:sub(1, 4), 'RIFF')
  eq(data:sub(9, 12), 'WAVE')
  eq(#data, 44 + 2 * 48000 * 2)
end)

test('a missing plugin is reported, not silently skipped', function()
  local _, problems = build('F2', { installed = { ['JS: SessionScope Sensor v0 (REAPER stand-in)'] = true, ReaEQ = true, ReaComp = true } })
  eq(#problems, 1)
  truthy(problems[1]:find('ReaSynth', 1, true), problems[1])
end)
