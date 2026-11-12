-- rpp_state_make.lua - part 1 of the project-state check (docs/research/rpp-state.md)
--
-- Builds a project with the ten gauntlet plug-ins (docs/research/gauntlet.md), one per track,
-- changes their settings (CHOWTapeModel: its factory program 10; the others: every fourth
-- parameter moved), writes down every parameter value REAPER reports, saves the project as
-- proof.rpp and quits. Everything is written into REAPER's resource folder, so run it only in an
-- isolated REAPER (its own settings folder), never the person's own:
--
--   reaper.exe -cfgfile <folder>\reaper.ini -newinst -nosplash -noactivate -new rpp_state_make.lua
--
-- Part 2 (rpp_state_reopen.lua) reopens the project; then `sessionscope bench --project
-- <folder>\proof.rpp --parameters` must show the same values as part 2 wrote down.
local dir = reaper.GetResourcePath()
local plugins = {
  "CHOWTapeModel", "Vital", "ValhallaVintageVerb", "Serum 2", "Pro-Q 4",
  "Ozone 12", "soothe2", "Gullfoss", "UADx LA-2A Tube Compressor", "Kontakt 8",
}

local function esc(s) return (s:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('[%c]', ' ')) end
local parts = {}
reaper.PreventUIRefresh(1)
for i, name in ipairs(plugins) do
  reaper.InsertTrackAtIndex(i - 1, false)
  local track = reaper.GetTrack(0, i - 1)
  reaper.GetSetMediaTrackInfo_String(track, "P_NAME", name, true)
  local fx = reaper.TrackFX_AddByName(track, "VST3:" .. name, false, -1)
  local entry = {'"track": "' .. esc(name) .. '"', '"added": ' .. tostring(fx >= 0)}
  if fx >= 0 then
    local _, fxname = reaper.TrackFX_GetFXName(track, fx, "")
    local own = reaper.TrackFX_GetNumParams(track, fx) - 3   -- REAPER adds Bypass, Wet and Delta at the end
    if name == "CHOWTapeModel" then
      reaper.TrackFX_SetPresetByIndex(track, fx, 10)
    else
      for p = 0, math.min(own, 400) - 1 do
        local _, pname = reaper.TrackFX_GetParamName(track, fx, p, "")
        if p % 4 == 1 and not pname:lower():find("bypass") then
          reaper.TrackFX_SetParamNormalized(track, fx, p, ((p * 37) % 100) / 100)
        end
      end
    end
    local values = {}
    for p = 0, own - 1 do
      local _, pname = reaper.TrackFX_GetParamName(track, fx, p, "")
      values[#values + 1] = string.format('["%s", %.6f]', esc(pname), reaper.TrackFX_GetParamNormalized(track, fx, p))
    end
    entry[#entry + 1] = '"fx": "' .. esc(fxname) .. '"'
    entry[#entry + 1] = '"params": [' .. table.concat(values, ", ") .. ']'
  end
  parts[#parts + 1] = "{" .. table.concat(entry, ", ") .. "}"
end
reaper.PreventUIRefresh(-1)

reaper.Main_SaveProjectEx(0, dir .. "/proof.rpp", 0)
local f = io.open(dir .. "/reaper_params.json", "w")
f:write("[\n" .. table.concat(parts, ",\n") .. "\n]\n")
f:close()
reaper.Main_OnCommand(40004, 0)   -- quit
