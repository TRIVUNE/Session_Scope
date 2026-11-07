local transport = require('transport')

local function run(samples)
  local st, events, passes = {}, {}, {}
  for _, s in ipairs(samples) do
    s.rate = s.rate or 1
    s.loop_start, s.loop_end = s.loop_start or 0, s.loop_end or 0
    s.tempo, s.ts_num, s.ts_den = s.tempo or 120, s.ts_num or 4, s.ts_den or 4
    local ev, pass = transport.step(st, s)
    for _, e in ipairs(ev) do events[#events + 1] = e.kind end
    passes[#passes + 1] = pass
  end
  return table.concat(events, ' '), passes
end

test('start and stop make one pass', function()
  local ev, passes = run({
    { t = 0, state = 0, pos2 = 0 }, { t = 0.1, state = 1, pos2 = 0 }, { t = 0.2, state = 1, pos2 = 0.1 },
    { t = 0.3, state = 0, pos2 = 0.1 },
  })
  eq(ev, 'start stop')
  eq(table.concat(passes, ','), '0,1,1,0')
end)

test('steady playback at any rate is not a seek', function()
  local ev = run({ { t = 0, state = 1, pos2 = 10 }, { t = 1, state = 1, pos2 = 11.5, rate = 1.5 }, { t = 2, state = 1, pos2 = 13, rate = 1.5 } })
  eq(ev, 'start')
end)

test('a jump is a seek and starts a new pass', function()
  local ev, passes = run({ { t = 0, state = 1, pos2 = 0 }, { t = 0.1, state = 1, pos2 = 0.1 }, { t = 0.2, state = 1, pos2 = 30 } })
  eq(ev, 'start seek')
  eq(passes[3], 2)
end)

test('wrapping at the loop end with repeat on is a loop, not a seek (T03)', function()
  local function s(t, pos, rep) return { t = t, state = 1, pos2 = pos, repeat_on = rep, loop_start = 64, loop_end = 96 } end
  local ev, passes = run({ s(0, 95.9, true), s(0.05, 95.95, true), s(0.1, 64.0, true) })
  eq(ev, 'start loop')
  eq(passes[3], 2)
  eq(run({ s(0, 95.9, false), s(0.1, 64, false) }), 'start seek', 'repeat off')
end)

test('recording, tempo and time signature changes are events', function()
  local ev = run({
    { t = 0, state = 1, pos2 = 0 },
    { t = 0.1, state = 5, pos2 = 0.1 },
    { t = 0.2, state = 5, pos2 = 0.2, tempo = 128 },
    { t = 0.3, state = 1, pos2 = 0.3, tempo = 128, ts_num = 3 },
  })
  eq(ev, 'start record tempo record_end timesig')
end)

test('pause ends playback like stop', function()
  local ev = run({ { t = 0, state = 1, pos2 = 0 }, { t = 0.1, state = 2, pos2 = 0.1 }, { t = 0.2, state = 1, pos2 = 0.1 } })
  eq(ev, 'start stop start')
end)
