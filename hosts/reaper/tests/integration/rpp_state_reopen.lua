-- rpp_state_reopen.lua - part 2 of the project-state check (docs/research/rpp-state.md)
--
-- Opens proof.rpp from REAPER's resource folder (made by rpp_state_make.lua), so REAPER gives
-- every plug-in its saved state back, writes down every parameter value REAPER now reports into
-- reaper_reopened.json, and quits. These are the values the bench must match. Isolated REAPER only.
local dir = reaper.GetResourcePath()
reaper.Main_openProject("noprompt:" .. dir .. "/proof.rpp")
local function esc(s) return (s:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('[%c]', ' ')) end
local parts = {}
for t = 0, reaper.CountTracks(0) - 1 do
  local track = reaper.GetTrack(0, t)
  local _, name = reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "", false)
  local values = {}
  local own = reaper.TrackFX_GetNumParams(track, 0) - 3   -- without REAPER's Bypass, Wet and Delta
  for p = 0, own - 1 do
    local _, pname = reaper.TrackFX_GetParamName(track, 0, p, "")
    values[#values + 1] = string.format('["%s", %.6f]', esc(pname), reaper.TrackFX_GetParamNormalized(track, 0, p))
  end
  parts[#parts + 1] = '{"track": "' .. esc(name) .. '", "params": [' .. table.concat(values, ", ") .. ']}'
end
local f = io.open(dir .. "/reaper_reopened.json", "w")
f:write("[\n" .. table.concat(parts, ",\n") .. "\n]\n")
f:close()
reaper.Main_OnCommand(40004, 0)   -- quit
