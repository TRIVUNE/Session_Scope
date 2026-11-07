-- @description SessionScope: export project inventory (tracks, FX, declared delay)
-- @version 0.1.0
-- @author TRIVUNE
-- @about
--   Spikes A and C: lists every track, FX (including input/monitoring FX and container contents),
--   GUIDs, bypass and offline state, and each FX's declared delay (PDC), then writes them to
--   <REAPER resource path>/SessionScope/inventory/<date-time>.json and prints a summary.
--   Reads only; the project is not changed.

local dir = ({ reaper.get_action_context() })[2]:match('^(.*[/\\])')
package.path = dir .. 'lib/?.lua;' .. package.path

local json = require('json')
local inventory = require('inventory')
local util = require('util')

local proj = reaper.EnumProjects(-1)
local inv = inventory.collect(reaper, proj)

local folder = util.join(util.join(reaper.GetResourcePath(), 'SessionScope'), 'inventory')
reaper.RecursiveCreateDirectory(folder, 0)
local path = util.join(folder, util.stamp() .. '.json')
util.write_file(path, json.encode(inv, '  ') .. '\n')

reaper.ShowConsoleMsg(inventory.summary(inv) .. 'Written to ' .. path .. '\n\n')
