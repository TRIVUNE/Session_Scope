-- Transport events from polled samples (spike B): start, stop, seek, loop wrap, record, tempo.
-- A pass is one continuous run of playback; every start, seek or loop wrap begins a new one, so
-- repeated bars are never stitched together (test T03).

local M = {}

M.JUMP_TOLERANCE = 0.25   -- seconds of movement that elapsed time does not explain

local function playing(state) return state & 1 == 1 end
local function recording(state) return state & 4 == 4 end

-- s = {t, state, pos2, rate, repeat_on, loop_start, loop_end, tempo, ts_num, ts_den}
-- Returns the events this sample reveals; st.pass is the pass the sample belongs to (0 = stopped).
function M.step(st, s)
  local ev, p = {}, st.prev
  local function emit(kind, detail) ev[#ev + 1] = { t = s.t, kind = kind, detail = detail or '' } end
  st.pass = st.pass or 0
  if not p then
    if playing(s.state) then st.pass = st.pass + 1; emit('start', 'already playing') end
  else
    local was, now = playing(p.state), playing(s.state)
    if now and not was then
      st.pass = st.pass + 1
      emit('start', string.format('at %.3f s', s.pos2))
    elseif was and not now then
      emit('stop', string.format('at %.3f s', p.pos2))
    elseif now then
      local dt = s.t - p.t
      local expected = p.pos2 + dt * (s.rate or 1)
      local jump = s.pos2 - expected
      if math.abs(jump) > M.JUMP_TOLERANCE then
        st.pass = st.pass + 1
        local wrapped = s.repeat_on and s.loop_end > s.loop_start and jump < 0
          and expected >= s.loop_end - M.JUMP_TOLERANCE
          and math.abs(s.pos2 - s.loop_start) <= dt * (s.rate or 1) + M.JUMP_TOLERANCE
        if wrapped then
          emit('loop', string.format('%.3f -> %.3f s', s.loop_end, s.loop_start))
        else
          emit('seek', string.format('%.3f -> %.3f s', p.pos2, s.pos2))
        end
      end
    end
    if recording(s.state) ~= recording(p.state) then emit(recording(s.state) and 'record' or 'record_end') end
    if s.tempo and p.tempo and math.abs(s.tempo - p.tempo) > 1e-6 then
      emit('tempo', string.format('%.3f -> %.3f bpm', p.tempo, s.tempo))
    end
    if s.ts_num and p.ts_num and (s.ts_num ~= p.ts_num or s.ts_den ~= p.ts_den) then
      emit('timesig', string.format('%d/%d -> %d/%d', p.ts_num, p.ts_den, s.ts_num, s.ts_den))
    end
  end
  st.prev = s
  return ev, playing(s.state) and st.pass or 0
end

return M
