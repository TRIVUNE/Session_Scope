-- Wrapping a track's VST3 effects in SessionScope Probes, and unwrapping them again.
--
-- A SessionScope Probe (native/vst3_probe) is a plug-in that holds another plug-in and times it,
-- so the HUD's Task Manager can show CPU per track and per plug-in. Putting a Probe in by hand
-- means: add the Probe, pick the plug-in in its window, set everything up again. This module does
-- it for effects that are already in a project: it swaps each one for a Probe that holds the same
-- plug-in with the same settings, and can swap them back.
--
-- How a swap works, all through documented ReaScript calls:
--   1. TrackFX_GetNamedConfigParm(track, fx, 'fx_ident') says which plug-in it is:
--          <path to the .vst3><<a number>{<class id, 32 hex digits>
--   2. 'vst_chunk' is the plug-in's saved state, base64 text. For a VST3, REAPER stores it as
--          4 bytes: length of the component state      (numbers are little endian: low byte first)
--          4 bytes: 1
--          the component state     (what the plug-in's audio half saved)
--          4 bytes: length of the controller state
--          4 bytes: 1 when there is one, else 0
--          the controller state    (what the plug-in's window half saved)
--      (not documented by REAPER: read from what it writes, with REAPER 7.83)
--   3. The Probe's own state names the plug-in inside and carries both halves:
--          4 bytes: 1 (version), then four "blobs" (4 bytes of length + the bytes):
--          path, class id, component state, controller state
--      (native/vst3_probe/sessionscope_probe.cpp reads and writes this)
--   4. A Probe is added, given that state wrapped the way step 2 describes, named
--      "Probe: <the plug-in's name>", moved to the plug-in's place, and the plug-in is removed.
--
-- Not carried over: automation envelopes and parameter links on the swapped plug-in (REAPER ties
-- them to the plug-in instance). Run it inside an undo block (the scripts do), try it on a copy.
-- Only the track's normal FX chain is handled: not input FX, take FX or FX inside containers.
--
-- `r` is the reaper API table, passed in so tests can use a fake host.

local M = {}

-- How TrackFX_AddByName finds the Probe, and how the Probe's renamed entries start.
M.PROBE_NAME = 'VST3:SessionScope Probe (TRIVUNE)'
M.PROBE_IDENT = 'SessionScopeProbe.vst3'     -- part of the Probe's own fx_ident
M.RENAME_PREFIX = 'Probe: '
M.STATE_VERSION = 1

-- ---- bytes ---------------------------------------------------------------------------------------

-- A whole number as 4 bytes, low byte first.
function M.u32(n)
  return string.char(n % 256, math.floor(n / 256) % 256, math.floor(n / 65536) % 256, math.floor(n / 16777216) % 256)
end

-- The whole number in the 4 bytes of `data` starting at position `at` (1 = first byte), or nil.
function M.read_u32(data, at)
  local a, b, c, d = data:byte(at, at + 3)
  if not d then return nil end
  -- 16777216.0 with the ".0": the sum is then worked out as a decimal number, which holds values
  -- up to 2^32 in every Lua. (The tests' Lua has 32-bit whole numbers; a plain sum went negative there.)
  return a + b * 256 + c * 65536 + d * 16777216.0
end

-- A blob: 4 bytes of length, then the bytes.
function M.blob(s) return M.u32(#s) .. s end

-- Reads a blob at position `at`. Returns the bytes and the position after it, or nil if cut short.
function M.read_blob(data, at)
  local n = M.read_u32(data, at)
  if not n or at + 3 + n > #data then return nil end
  return data:sub(at + 4, at + 3 + n), at + 4 + n
end

local B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

-- Bytes to base64 text (REAPER passes plug-in states as base64).
function M.base64(data)
  local out = {}
  for i = 1, #data, 3 do
    local a, b, c = data:byte(i, i + 2)
    local n = a * 65536 + (b or 0) * 256 + (c or 0)                   -- three bytes as one 24-bit number
    local c1, c2 = math.floor(n / 262144) % 64, math.floor(n / 4096) % 64
    local c3, c4 = math.floor(n / 64) % 64, n % 64                    -- cut into four 6-bit numbers
    out[#out + 1] = B64:sub(c1 + 1, c1 + 1) .. B64:sub(c2 + 1, c2 + 1) ..
      (b and B64:sub(c3 + 1, c3 + 1) or '=') .. (c and B64:sub(c4 + 1, c4 + 1) or '=')   -- '=' pads a short end
  end
  return table.concat(out)
end

-- Base64 text back to bytes. Characters that aren't base64 (line breaks, '=') are skipped.
function M.unbase64(text)
  local out, n, bits = {}, 0, 0
  for ch in text:gmatch('[A-Za-z0-9+/]') do
    n = (n * 64 + B64:find(ch, 1, true) - 1) % 16777216               -- collect 6 bits at a time
    bits = bits + 6
    if bits >= 8 then                                                 -- a whole byte is ready
      bits = bits - 8
      out[#out + 1] = string.char(math.floor(n / 2 ^ bits) % 256)
    end
  end
  return table.concat(out)
end

-- ---- states --------------------------------------------------------------------------------------

-- REAPER's VST3 chunk (step 2 above) into its two halves. Returns component, controller (either
-- may be ''), or nil if the bytes don't have that shape.
function M.split_reaper_chunk(chunk)
  local n = M.read_u32(chunk, 1)
  if not n or 8 + n > #chunk then return nil end
  local component = chunk:sub(9, 8 + n)
  local m = M.read_u32(chunk, 9 + n)                                  -- the controller's length, if the chunk goes on
  if not m then return component, '' end
  if 16 + n + m > #chunk then return nil end
  return component, chunk:sub(17 + n, 16 + n + m)
end

-- The two halves back into REAPER's chunk.
function M.join_reaper_chunk(component, controller)
  controller = controller or ''
  return M.u32(#component) .. M.u32(1) .. component .. M.u32(#controller) .. M.u32(#controller > 0 and 1 or 0) .. controller
end

-- The Probe's own state (step 3 above).
function M.probe_state(path, class_id, component, controller)
  return M.u32(M.STATE_VERSION) .. M.blob(path) .. M.blob(class_id) .. M.blob(component or '') .. M.blob(controller or '')
end

-- The Probe's state back into its parts: { path, class_id, component, controller }, or nil.
function M.parse_probe_state(state)
  if M.read_u32(state, 1) ~= M.STATE_VERSION then return nil end
  local out, at = {}, 5
  for _, key in ipairs({ 'path', 'class_id', 'component', 'controller' }) do
    out[key], at = M.read_blob(state, at)
    if not out[key] then return nil end
  end
  return out
end

-- 'fx_ident' of a VST3 into its .vst3 path and class id (32 hex digits), or nil.
function M.parse_ident(ident)
  if not ident then return nil end
  local path, class_id = ident:match('^(.-)<%d*{(%x+)$')
  if not path or #class_id ~= 32 then return nil end
  return path, class_id:upper()
end

-- ---- REAPER --------------------------------------------------------------------------------------

-- One TrackFX_GetNamedConfigParm value, or nil if REAPER does not have it for this FX.
local function named(r, track, fx, key)
  local ok, v = r.TrackFX_GetNamedConfigParm(track, fx, key)
  if ok then return v end
  return nil
end

-- Is the FX at `fx` a SessionScope Probe?
function M.is_probe(r, track, fx)
  local ident = named(r, track, fx, 'fx_ident') or ''
  return ident:find(M.PROBE_IDENT, 1, true) ~= nil
end

-- The state of the Probe at `fx`, parsed (see parse_probe_state), or nil.
function M.read_probe(r, track, fx)
  local chunk = named(r, track, fx, 'vst_chunk')
  local state = chunk and M.split_reaper_chunk(M.unbase64(chunk))
  return state and M.parse_probe_state(state)
end

-- Swaps the VST3 effect at chain position `fx` for a Probe holding it. Returns true, or false and
-- why not. The chain keeps its length and order. SessionScope's own plug-ins are left alone: a
-- Probe is one already, and the Sensor belongs on the master as it is. `even_the_sensor` = true
-- wraps the Sensor all the same: the validation run uses it as the one VST3 effect that is
-- installed wherever this repository is built.
function M.wrap(r, track, fx, even_the_sensor)
  local kind = named(r, track, fx, 'fx_type')
  if kind ~= 'VST3' then return false, 'not a VST3 effect (' .. tostring(kind) .. ')' end   -- VST3i (instruments), VST2, JS, CLAP...
  local ident = named(r, track, fx, 'fx_ident') or ''
  local own = ident:find('SessionScope', 1, true) ~= nil
  if own and (M.is_probe(r, track, fx) or not even_the_sensor) then return false, 'a SessionScope plug-in' end
  local path, class_id = M.parse_ident(ident)
  if not path then return false, 'REAPER did not say which plug-in this is' end
  local chunk = named(r, track, fx, 'vst_chunk')
  local component, controller = '', ''
  if chunk and chunk ~= '' then
    component, controller = M.split_reaper_chunk(M.unbase64(chunk))
    if not component then return false, 'its saved state has a shape this script does not know' end
  end
  local name = named(r, track, fx, 'fx_name') or select(2, r.TrackFX_GetFXName(track, fx))
  local shown = select(2, r.TrackFX_GetFXName(track, fx))             -- what the chain shows (a rename included)
  local enabled, offline = r.TrackFX_GetEnabled(track, fx), r.TrackFX_GetOffline(track, fx)

  local probe = r.TrackFX_AddByName(track, M.PROBE_NAME, false, -1)   -- -1: always add a new one, at the end
  if probe < 0 then return false, 'SessionScope Probe is not installed (run: sessionscope install, then rescan plug-ins)' end
  local state = M.probe_state(path, class_id, component, controller)
  r.TrackFX_SetNamedConfigParm(track, probe, 'vst_chunk', M.base64(M.join_reaper_chunk(state, '')))
  local loaded = M.read_probe(r, track, probe)                        -- ask the Probe what it holds now
  if not loaded or loaded.path ~= path then
    r.TrackFX_Delete(track, probe)
    return false, 'the Probe could not load it'
  end
  -- The name keeps what REAPER calls the plug-in: unwrap() finds the plug-in again by it.
  r.TrackFX_SetNamedConfigParm(track, probe, 'renamed_name', M.RENAME_PREFIX .. name)
  if not enabled then r.TrackFX_SetEnabled(track, probe, false) end
  if offline then r.TrackFX_SetOffline(track, probe, true) end
  r.TrackFX_CopyToTrack(track, probe, track, fx, true)                -- true = move: the Probe takes the place...
  r.TrackFX_Delete(track, fx + 1)                                     -- ...and the plug-in, pushed one down, goes
  return true, shown
end

-- Swaps the Probe at chain position `fx` back for the plug-in it holds. Returns true, or false and why not.
function M.unwrap(r, track, fx)
  if not M.is_probe(r, track, fx) then return false, 'not a Probe' end
  local held = M.read_probe(r, track, fx)
  if not held or held.path == '' then return false, 'this Probe holds no plug-in' end
  local shown = select(2, r.TrackFX_GetFXName(track, fx)) or ''
  local name = shown:sub(1, #M.RENAME_PREFIX) == M.RENAME_PREFIX and shown:sub(#M.RENAME_PREFIX + 1) or nil
  if not name then return false, 'this Probe was not made by the wrap script (its name does not say which plug-in to add)' end
  local enabled, offline = r.TrackFX_GetEnabled(track, fx), r.TrackFX_GetOffline(track, fx)

  local plain = r.TrackFX_AddByName(track, name, false, -1)
  if plain < 0 then return false, 'REAPER could not add ' .. name end
  local _, class_id = M.parse_ident(named(r, track, plain, 'fx_ident'))
  if class_id ~= held.class_id:upper() then                          -- another plug-in with the same name
    r.TrackFX_Delete(track, plain)
    return false, name .. ' is now a different plug-in'
  end
  if held.component ~= '' or held.controller ~= '' then
    r.TrackFX_SetNamedConfigParm(track, plain, 'vst_chunk', M.base64(M.join_reaper_chunk(held.component, held.controller)))
  end
  if not enabled then r.TrackFX_SetEnabled(track, plain, false) end
  if offline then r.TrackFX_SetOffline(track, plain, true) end
  r.TrackFX_CopyToTrack(track, plain, track, fx, true)
  r.TrackFX_Delete(track, fx + 1)
  return true, name
end

-- The tracks a script should work on: the selected ones, or every track and the master when none
-- is selected.
function M.target_tracks(r, proj)
  local tracks = {}
  for i = 0, r.CountSelectedTracks2(proj, true) - 1 do tracks[#tracks + 1] = r.GetSelectedTrack2(proj, i, true) end   -- true: the master counts
  if #tracks > 0 then return tracks, 'selected tracks' end
  tracks[1] = r.GetMasterTrack(proj)
  for i = 0, r.CountTracks(proj) - 1 do tracks[#tracks + 1] = r.GetTrack(proj, i) end
  return tracks, 'all tracks'
end

-- Runs `swap` (M.wrap or M.unwrap) on every FX of the tracks. Returns { done = n, skipped = {text...} }.
function M.each_fx(r, tracks, swap)
  local result = { done = 0, skipped = {} }
  for _, track in ipairs(tracks) do
    local _, track_name = r.GetTrackName(track)
    for fx = 0, r.TrackFX_GetCount(track) - 1 do                      -- a swap keeps the chain's length, so plain counting works
      local ok, text = swap(r, track, fx)
      if ok then
        result.done = result.done + 1
      elseif text ~= 'not a Probe' and text ~= 'a SessionScope plug-in' then   -- those two are simply not ours to swap
        local _, fx_name = r.TrackFX_GetFXName(track, fx)
        result.skipped[#result.skipped + 1] = track_name .. ' / ' .. fx_name .. ': ' .. text
      end
    end
  end
  return result
end

return M
