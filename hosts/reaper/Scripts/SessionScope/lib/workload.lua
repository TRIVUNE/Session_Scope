-- Ground truth for spike S2: the CPU time the SessionScope Workload JSFX burns in a block.
-- Mirrors the @block code of hosts/reaper/Effects/SessionScope/SessionScope Workload.jsfx.

local M = {}

M.NAME = 'SessionScope Workload'

-- p = {cost_us, mode, from_qn, to_qn, step_us, step_qn} (slider values in order)
function M.cost(p, qn, frames)
  local active = qn >= p.from_qn and (p.to_qn <= 0 or qn < p.to_qn)
  if not active then return 0 end
  local c = p.cost_us
  if p.step_us > 0 and p.step_qn > 0 then c = c + p.step_us * math.floor((qn - p.from_qn) / p.step_qn) end
  if p.mode == 1 then c = c * frames / 256 end
  return c
end

local KEYS = { 'cost_us', 'mode', 'from_qn', 'to_qn', 'step_us', 'step_qn' }

-- Enabled Workload instances in the project, with their current slider values.
function M.scan(r, proj)
  local out = {}
  local function chain(tr)
    if r.GetMediaTrackInfo_Value(tr, 'I_FXEN') == 0 then return end
    for fx = 0, r.TrackFX_GetCount(tr) - 1 do
      local _, name = r.TrackFX_GetFXName(tr, fx)
      if name:find(M.NAME, 1, true) and r.TrackFX_GetEnabled(tr, fx) and not r.TrackFX_GetOffline(tr, fx) then
        local p = {}
        for i, k in ipairs(KEYS) do p[k] = (r.TrackFX_GetParam(tr, fx, i - 1)) end
        out[#out + 1] = p
      end
    end
  end
  chain(r.GetMasterTrack(proj))
  for i = 0, r.CountTracks(proj) - 1 do chain(r.GetTrack(proj, i)) end
  return out
end

-- Total known cost of all instances for a block at position qn with this many frames, in us.
function M.total(list, qn, frames)
  local sum = 0
  for _, p in ipairs(list) do sum = sum + M.cost(p, qn, frames) end
  return sum
end

return M
