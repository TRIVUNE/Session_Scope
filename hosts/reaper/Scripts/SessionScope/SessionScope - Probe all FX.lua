-- @description SessionScope: put every VST3 effect in a Probe (CPU per track and plug-in)
-- @version 0.1.0
-- @author TRIVUNE
-- @about
--   Swaps every VST3 effect on the selected tracks (or on all tracks and the master, when no track
--   is selected) for a SessionScope Probe that holds the same plug-in with the same settings. The
--   Probe times the plug-in, and the SessionScope HUD's Task Manager then shows CPU per track and
--   per plug-in. "SessionScope - Remove probes" swaps them back.
--
--   Kept: the plug-in's settings, its place in the chain, bypass and offline state, its latency.
--   Not kept: automation envelopes and parameter links on the swapped plug-ins. One undo step
--   (Ctrl+Z) brings everything back, envelopes included. Try it on a copy of the project first.
--   Left alone: instruments, VST2, CLAP and JS effects, input FX, take FX, FX inside containers.
--   A plug-in with a sidechain input or extra outputs gets those back after the project is saved
--   and reopened (REAPER reads a plug-in's inputs and outputs when it loads it).
--
--   Needs the Probe installed: build/engine/sessionscope install, then rescan plug-ins in REAPER.

local dir = ({ reaper.get_action_context() })[2]:match('^(.*[/\\])')
package.path = dir .. 'lib/?.lua;' .. package.path

local probes = require('probes')

local proj = reaper.EnumProjects(-1)                                  -- the project in front
local tracks, which = probes.target_tracks(reaper, proj)

reaper.Undo_BeginBlock2(proj)                                         -- everything below is one undo step
reaper.PreventUIRefresh(1)                                            -- don't redraw after every single swap
local result = probes.each_fx(reaper, tracks, probes.wrap)
reaper.PreventUIRefresh(-1)
reaper.Undo_EndBlock2(proj, 'SessionScope: put VST3 effects in Probes', -1)

local lines = { ('SessionScope: %d effect(s) on %s are now in Probes.'):format(result.done, which) }
for _, text in ipairs(result.skipped) do lines[#lines + 1] = '  left alone: ' .. text end
reaper.ShowConsoleMsg(table.concat(lines, '\n') .. '\n\n')
