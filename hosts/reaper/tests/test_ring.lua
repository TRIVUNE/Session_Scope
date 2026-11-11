-- lib/ring.lua: reading the sensor ring out of gmem, with wrap-around, resets and lost records.

local ring = require('ring')

-- A writer that does what the @block section of SessionScope Sensor.jsfx does.
local function sensor(cap)
  local g = setmetatable({}, { __index = function() return 0 end })
  local w = { g = g, cap = cap or 8, id = 4242 }
  function w.init()
    g[0], g[1], g[2], g[3], g[4], g[9] = ring.MAGIC, ring.LAYOUT, 0, w.cap, #ring.FIELDS, 0.1
    g[5] = w.id
  end
  function w.block(t, pos)
    local n = g[2]
    local p = ring.HDR + (n % w.cap) * #ring.FIELDS
    local vals = { t, 256, 48000, 1, pos or t, (pos or t) * 2, 120, 4, 4, 1.5, n, w.id }
    for i, v in ipairs(vals) do g[p + i - 1] = v end
    g[2] = n + 1
    g[6] = t
  end
  function w.read(i) return g[i] end
  return w
end

test('no sensor: nothing, and no header', function()
  local w = sensor()
  local rd = ring.new(w.read)
  local recs, hd = ring.drain(rd)
  eq(#recs, 0)
  eq(hd, nil)
end)

test('first drain skips history, later drains return new records in order', function()
  local w = sensor()
  w.init()
  for i = 1, 5 do w.block(i) end
  local rd = ring.new(w.read)
  eq(#ring.drain(rd), 0, 'history')
  for i = 6, 8 do w.block(i) end
  local recs, hd = ring.drain(rd)
  eq(#recs, 3)
  eq(recs[1].t, 6)
  eq(recs[3].seq, 7)
  eq(recs[1].frames, 256)
  eq(hd.owner, 4242)
  eq(rd.dropped, 0)
end)

test('falling behind by more than the ring holds counts the lost records', function()
  local w = sensor(8)
  w.init()
  local rd = ring.new(w.read)
  ring.drain(rd)
  for i = 1, 20 do w.block(i) end
  local recs = ring.drain(rd)
  -- The newest cap - 1 records are safe; record 12's slot may be in use for record 20.
  eq(#recs, 7)
  eq(recs[1].t, 14)
  eq(rd.dropped, 13)
end)

test('records the writer may have reached during the copy are dropped, not returned', function()
  local w = sensor(8)
  w.init()
  local rd = ring.new(w.read)
  ring.drain(rd)
  for i = 1, 4 do w.block(i) end
  -- After the first record is copied, the writer races ahead by five: record 8 reuses record 0's
  -- slot and record 9 may be half way through record 1's.
  local reads = 0
  rd.read = function(i)
    reads = reads + 1
    if reads == 10 + #ring.FIELDS then for k = 5, 9 do w.block(k) end end
    return w.read(i)
  end
  local recs = ring.drain(rd)
  eq(#recs, 2)
  eq(recs[1].seq, 2)
  eq(recs[2].t, 4)
  eq(rd.dropped, 2)
end)

test('a torn record (sequence mismatch) is dropped', function()
  local w = sensor(8)
  w.init()
  local rd = ring.new(w.read)
  ring.drain(rd)
  w.block(1)
  w.block(2)
  w.g[ring.HDR + 1 * #ring.FIELDS + 10] = 99
  local recs = ring.drain(rd)
  eq(#recs, 1)
  eq(rd.dropped, 1)
end)

test('a restarted sensor is detected and read from the start', function()
  local w = sensor(8)
  w.init()
  for i = 1, 6 do w.block(i) end
  local rd = ring.new(w.read)
  ring.drain(rd)
  w.init()
  w.block(100)
  w.block(101)
  local recs = ring.drain(rd)
  eq(rd.resets, 1)
  eq(#recs, 2)
  eq(recs[1].t, 100)
end)

test('max limits one drain; the rest comes next time', function()
  local w = sensor(16)
  w.init()
  local rd = ring.new(w.read)
  ring.drain(rd)
  for i = 1, 10 do w.block(i) end
  eq(#ring.drain(rd, 4), 4)
  eq(#ring.drain(rd), 6)
end)

test('the layout matches the JSFX writer', function()
  local src = read_file(__repo .. '/hosts/reaper/Effects/SessionScope/SessionScope Sensor.jsfx')
  eq(src:match('options:gmem=(%S+)'), ring.SEGMENT, 'segment')
  eq(tonumber(src:match('MAGIC = (%d+);')), ring.MAGIC, 'magic')
  eq(tonumber(src:match('LAYOUT = (%d+);')), ring.LAYOUT, 'layout')
  eq(tonumber(src:match('HDR = (%d+);')), ring.HDR, 'header size')
  eq(tonumber(src:match('REC = (%d+);')), #ring.FIELDS, 'record size')
  local writes = { [0] = src:match('gmem%[p%] = ([%w_]+);') }
  for k, v in src:gmatch('gmem%[p %+ (%d+)%] = ([%w_]+);') do writes[tonumber(k)] = v end
  local expected = { t = 't0', frames = 'samplesblock', srate = 'srate', play_state = 'play_state', play_pos = 'play_position',
    qn = 'beat_position', tempo = 'tempo', ts_num = 'ts_num', ts_den = 'ts_denom', self_us = 'prev_cost', seq = 'n', instance = 'my_id' }
  for i, name in ipairs(ring.FIELDS) do eq(writes[i - 1], expected[name], 'field ' .. name) end
  for name, slot in pairs(ring.H) do
    if name ~= 'magic' and name ~= 'layout' and name ~= 'cap' and name ~= 'rec' and name ~= 'version' then
      truthy(src:find('gmem%[' .. slot .. '%]'), 'header slot ' .. name .. ' is written')
    end
  end
end)
