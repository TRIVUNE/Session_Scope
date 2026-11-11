-- @description SessionScope: take every plug-in out of its Probe again
-- @version 0.1.0
-- @author TRIVUNE
-- @about
--   The opposite of "SessionScope - Probe all FX": swaps every SessionScope Probe on the selected
--   tracks (or on all tracks and the master, when no track is selected) back for the plug-in it
--   holds, with the settings the plug-in has now. Bypass and offline state and the place in the
--   chain are kept; automation envelopes on a Probe are not (one undo step brings them back).
--   A Probe that was added by hand, not by the script, is left alone: its name doesn't say which
--   plug-in to put back.

local dir = ({ reaper.get_action_context() })[2]:match('^(.*[/\\])')
package.path = dir .. 'lib/?.lua;' .. package.path

local probes = require('probes')

local proj = reaper.EnumProjects(-1)                                  -- the project in front
local tracks, which = probes.target_tracks(reaper, proj)

reaper.Undo_BeginBlock2(proj)                                         -- everything below is one undo step
reaper.PreventUIRefresh(1)                                            -- don't redraw after every single swap
local result = probes.each_fx(reaper, tracks, probes.unwrap)
reaper.PreventUIRefresh(-1)
reaper.Undo_EndBlock2(proj, 'SessionScope: take plug-ins out of Probes', -1)

local lines = { ('SessionScope: %d plug-in(s) on %s are out of their Probes.'):format(result.done, which) }
for _, text in ipairs(result.skipped) do lines[#lines + 1] = '  left alone: ' .. text end
reaper.ShowConsoleMsg(table.concat(lines, '\n') .. '\n\n')
