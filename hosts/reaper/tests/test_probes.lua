-- lib/probes.lua: the byte formats (REAPER's VST3 chunk, the Probe's state) and the wrap / unwrap
-- swaps, against a small pretend FX chain. The real thing is checked inside REAPER by
-- tests/integration/validate.lua (the "probe" step).

local probes = require('probes')

-- Bytes as hex text, for readable failures.
local function hex(s)
  return (s:gsub('.', function(c) return string.format('%02X', c:byte()) end))
end

test('numbers and blobs are little endian', function()
  eq(hex(probes.u32(1)), '01000000')
  eq(hex(probes.u32(0x12345678)), '78563412')
  eq(probes.read_u32(probes.u32(3000000000), 1), 3000000000, 'a number above 2^31 survives')
  eq(probes.read_u32('abc', 1), nil, 'too short')
  eq(hex(probes.blob('AB')), '020000004142')
  local bytes, after = probes.read_blob('xx' .. probes.blob('hello') .. 'rest', 3)
  eq(bytes, 'hello')
  eq(after, 12, 'the position after the blob')
  eq(probes.read_blob(probes.u32(10) .. 'short', 1), nil, 'a blob cut short')
end)

test('base64 both ways, every ending length', function()
  eq(probes.base64(''), '')
  eq(probes.base64('f'), 'Zg==')
  eq(probes.base64('fo'), 'Zm8=')
  eq(probes.base64('foo'), 'Zm9v')
  eq(probes.base64('foobar'), 'Zm9vYmFy')
  local all = {}
  for i = 0, 255 do all[#all + 1] = string.char(i) end
  all = table.concat(all)
  for cut = 253, 256 do
    local data = all:sub(1, cut)
    eq(hex(probes.unbase64(probes.base64(data))), hex(data), 'round trip of ' .. cut .. ' bytes')
  end
  eq(probes.unbase64('Zm9v\nYmFy'), 'foobar', 'line breaks are skipped')
end)

test("REAPER's VST3 chunk splits into component and controller state", function()
  -- What REAPER 7.83 returned for an empty Probe: 20 bytes of state, no controller state.
  local empty = probes.u32(20) .. probes.u32(1) .. probes.u32(1) .. string.rep('\0', 16) .. probes.u32(0) .. probes.u32(0)
  local component, controller = probes.split_reaper_chunk(empty)
  eq(#component, 20)
  eq(controller, '')
  eq(hex(probes.join_reaper_chunk(component, controller)), hex(empty), 'and joins back to the same bytes')

  local both = probes.join_reaper_chunk('COMPONENT', 'CTRL')
  component, controller = probes.split_reaper_chunk(both)
  eq(component, 'COMPONENT')
  eq(controller, 'CTRL')
  eq(probes.split_reaper_chunk(probes.u32(99) .. probes.u32(1) .. 'too short'), nil, 'a length that runs past the end')
  eq(probes.split_reaper_chunk(''), nil)
end)

test("the Probe's state names the plug-in and carries both halves", function()
  local state = probes.probe_state('C:\\VST3\\Thing.vst3', '0123456789ABCDEF0123456789ABCDEF', 'comp', 'ctrl')
  eq(hex(state:sub(1, 4)), '01000000', 'version 1 first')
  local parsed = probes.parse_probe_state(state)
  eq(parsed.path, 'C:\\VST3\\Thing.vst3')
  eq(parsed.class_id, '0123456789ABCDEF0123456789ABCDEF')
  eq(parsed.component, 'comp')
  eq(parsed.controller, 'ctrl')
  eq(probes.parse_probe_state(probes.u32(2) .. state:sub(5)), nil, 'another version is not guessed at')
  eq(probes.parse_probe_state(state:sub(1, -3)), nil, 'cut short')
  -- An empty Probe: version and four empty blobs, the 20 bytes seen above.
  local none = probes.parse_probe_state(probes.probe_state('', '', '', ''))
  eq(none.path, '')
end)

test('fx_ident gives the path and the class id', function()
  local path, id = probes.parse_ident('C:\\Program Files\\Common Files\\VST3\\Crisp.vst3\\Contents\\x86_64-win\\Crisp.vst3<878385771{4372697370506c7567696e527664482e')
  eq(path, 'C:\\Program Files\\Common Files\\VST3\\Crisp.vst3\\Contents\\x86_64-win\\Crisp.vst3')
  eq(id, '4372697370506C7567696E527664482E', 'upper case')
  eq(probes.parse_ident('reaeq.dll'), nil, 'not a VST3 ident')
  eq(probes.parse_ident('C:\\x.vst3<1{1234'), nil, 'a class id must be 32 digits')
  eq(probes.parse_ident(nil), nil)
end)

-- A pretend REAPER with one track: just the FX calls probes.lua uses. A "Probe" entry keeps the
-- chunk it is given, the way the real Probe hands its state back.
local function pretend(chain, o)
  o = o or {}
  local r, track = {}, { name = 'Vox', fx = chain }
  local function find(a) return track.fx[a + 1] end
  function r.TrackFX_GetCount() return #track.fx end
  function r.TrackFX_GetFXName(_, a) local f = find(a) return f ~= nil, f and (f.renamed or f.name) or '' end
  function r.TrackFX_GetEnabled(_, a) return find(a).enabled ~= false end
  function r.TrackFX_GetOffline(_, a) return find(a).offline == true end
  function r.TrackFX_SetEnabled(_, a, v) find(a).enabled = v end
  function r.TrackFX_SetOffline(_, a, v) find(a).offline = v end
  function r.TrackFX_Delete(_, a) table.remove(track.fx, a + 1) return true end
  function r.TrackFX_GetNamedConfigParm(_, a, key)
    local f = find(a)
    if not f then return false, '' end
    if key == 'fx_type' then return true, f.type end
    if key == 'fx_ident' then return f.ident ~= nil, f.ident or '' end
    if key == 'fx_name' then return true, f.name end
    if key == 'vst_chunk' then return f.chunk ~= nil, f.chunk and probes.base64(f.chunk) or '' end
    return false, ''
  end
  function r.TrackFX_SetNamedConfigParm(_, a, key, value)
    local f = find(a)
    if key == 'renamed_name' then f.renamed = value return true end
    if key == 'vst_chunk' then
      if f.refuses then return true end                              -- like a Probe that can't load the plug-in
      f.chunk = probes.unbase64(value)
      return true
    end
    return false
  end
  function r.TrackFX_AddByName(_, name)
    local known = o.installed and o.installed[name]
    if name == probes.PROBE_NAME then
      if o.no_probe then return -1 end
      known = { name = 'VST3: SessionScope Probe (TRIVUNE)', type = 'VST3', refuses = o.probe_refuses,
        ident = 'C:\\VST3\\SessionScopeProbe.vst3\\Contents\\x86_64-win\\SessionScopeProbe.vst3<1{8B2E807BC9014C31A9788B53B4EA30F2',
        chunk = probes.join_reaper_chunk(probes.probe_state('', '', '', ''), '') }
    end
    if not known then return -1 end
    local copy = {}
    for k, v in pairs(known) do copy[k] = v end
    track.fx[#track.fx + 1] = copy
    return #track.fx - 1
  end
  function r.TrackFX_CopyToTrack(_, from, _, to, move)
    assert(move, 'the pretend chain only moves')
    local f = table.remove(track.fx, from + 1)
    table.insert(track.fx, to + 1, f)
  end
  function r.GetTrackName() return true, track.name end
  return r, track
end

local EQ = { name = 'VST3: Fancy EQ (Maker)', type = 'VST3', ident = 'C:\\VST3\\Fancy EQ.vst3<12{00112233445566778899AABBCCDDEEFF',
  chunk = probes.join_reaper_chunk('eq settings', 'eq window') }
local COMP = { name = 'VST3: Comp (Maker)', type = 'VST3', ident = 'C:\\VST3\\Comp.vst3<7{FFEEDDCCBBAA99887766554433221100',
  chunk = probes.join_reaper_chunk('comp settings', ''), enabled = false }

local function copy(t)
  local out = {}
  for k, v in pairs(t) do out[k] = v end
  return out
end

test('wrap: the Probe takes the place, holds the plug-in and its settings', function()
  local r, track = pretend({ { name = 'JS: Gain', type = 'JS' }, copy(EQ), copy(COMP) })
  local ok, what = probes.wrap(r, track, 1)
  truthy(ok, what)
  eq(#track.fx, 3, 'the chain keeps its length')
  eq(track.fx[1].name, 'JS: Gain', 'the FX before is untouched')
  eq(track.fx[3].name, COMP.name, 'the FX after is untouched')
  eq(track.fx[2].renamed, 'Probe: VST3: Fancy EQ (Maker)')
  truthy(probes.is_probe(r, track, 1))
  local held = probes.read_probe(r, track, 1)
  eq(held.path, 'C:\\VST3\\Fancy EQ.vst3')
  eq(held.class_id, '00112233445566778899AABBCCDDEEFF')
  eq(held.component, 'eq settings')
  eq(held.controller, 'eq window')
end)

test('wrap keeps bypass; unwrap puts the same plug-in back', function()
  local r, track = pretend({ copy(EQ), copy(COMP) }, { installed = { [EQ.name] = EQ, [COMP.name] = COMP } })
  truthy(probes.wrap(r, track, 1))
  eq(track.fx[2].enabled, false, 'a bypassed plug-in gives a bypassed Probe')
  local ok, what = probes.unwrap(r, track, 1)
  truthy(ok, what)
  eq(#track.fx, 2)
  eq(track.fx[2].name, COMP.name)
  eq(track.fx[2].enabled, false, 'and bypassed again after unwrapping')
  local component, controller = probes.split_reaper_chunk(track.fx[2].chunk)
  eq(component, 'comp settings')
  eq(controller, '')
end)

test('what is left alone, and why', function()
  local r, track = pretend({ { name = 'JS: Gain', type = 'JS' }, { name = 'VSTi: Synth', type = 'VST3i' }, copy(EQ) }, { no_probe = true })
  local ok, why = probes.wrap(r, track, 0)
  eq(ok, false)
  truthy(why:find('not a VST3 effect'), why)
  eq(select(1, probes.wrap(r, track, 1)), false, 'an instrument')
  ok, why = probes.wrap(r, track, 2)
  eq(ok, false)
  truthy(why:find('not installed'), why)
  eq(#track.fx, 3, 'nothing was changed')

  r, track = pretend({ copy(EQ) }, { probe_refuses = true })
  ok, why = probes.wrap(r, track, 0)
  eq(ok, false)
  truthy(why:find('could not load'), why)
  eq(#track.fx, 1, 'the Probe that failed was removed again')
  eq(track.fx[1].name, EQ.name)

  ok, why = probes.unwrap(r, track, 0)
  eq(ok, false)
  eq(why, 'not a Probe')

  -- SessionScope's own Sensor is left alone, unless asked for (the validation run does).
  local SENSOR = { name = 'VST3: SessionScope Sensor (TRIVUNE)', type = 'VST3',
    ident = 'C:\\VST3\\SessionScope.vst3\\Contents\\x86_64-win\\SessionScope.vst3<1{5E55F0A17C0D4B6E9A2E1D430B5C8E77' }
  r, track = pretend({ copy(SENSOR) })
  ok, why = probes.wrap(r, track, 0)
  eq(ok, false)
  eq(why, 'a SessionScope plug-in')
  truthy(probes.wrap(r, track, 0, true), 'wrapped when asked for')
  eq(select(1, probes.wrap(r, track, 0, true)), false, 'but never a Probe inside a Probe')
end)

test('each_fx swaps a whole chain and reports what it skipped', function()
  local r, track = pretend({ copy(EQ), { name = 'VST: Old (Maker)', type = 'VST' }, copy(COMP) }, { installed = { [EQ.name] = EQ, [COMP.name] = COMP } })
  local result = probes.each_fx(r, { track }, probes.wrap)
  eq(result.done, 2)
  eq(#result.skipped, 1)
  truthy(result.skipped[1]:find('Vox / VST: Old %(Maker%): not a VST3 effect'), result.skipped[1])
  truthy(probes.is_probe(r, track, 0) and probes.is_probe(r, track, 2))

  local again = probes.each_fx(r, { track }, probes.wrap)       -- a second run finds nothing left to wrap
  eq(again.done, 0)
  eq(#again.skipped, 1, 'Probes themselves are not reported as skipped')

  local back = probes.each_fx(r, { track }, probes.unwrap)
  eq(back.done, 2)
  eq(track.fx[1].name, EQ.name)
  eq(track.fx[3].name, COMP.name)
  eq(#track.fx, 3)
end)
