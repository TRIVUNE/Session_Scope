-- @description SessionScope: build the fixture projects (F1-F5, F7)
-- @version 0.1.0
-- @author TRIVUNE
-- @about
--   Builds every fixture in lib/fixtures.lua, each in a new project tab, saves it under
--   <REAPER resource path>/SessionScope/fixtures/<id>/<id>.rpp and closes the tab again.
--   Projects you already have open are left alone. A manifest.json records what was built and
--   anything that could not be set up (for example a plugin that is not installed).

local dir = ({ reaper.get_action_context() })[2]:match('^(.*[/\\])')
package.path = dir .. 'lib/?.lua;' .. package.path

local fixtures = require('fixtures')
local util = require('util')

local r = reaper
local root = util.join(util.join(r.GetResourcePath(), 'SessionScope'), 'fixtures')
local ids = {}
for _, f in ipairs(fixtures.list) do ids[#ids + 1] = f.id end

if r.MB(string.format('Build %d fixture projects (%s) into\n%s\n\nEach is built in a new project tab, saved and closed. Your open projects are not changed.',
  #ids, table.concat(ids, ', '), root), 'SessionScope fixtures', 1) ~= 1 then
  return
end

local ok, manifest = pcall(fixtures.build_all, r, root)
if not ok then
  r.MB('Nothing was built: ' .. tostring(manifest), 'SessionScope fixtures', 0)
  return
end
local lines = { 'SessionScope fixtures built in ' .. root }
for _, x in ipairs(manifest.fixtures) do
  lines[#lines + 1] = string.format('  %-4s %s%s', x.id, x.title, #x.problems > 0 and ('\n       ! ' .. table.concat(x.problems, '\n       ! ')) or '')
end
r.ShowConsoleMsg(table.concat(lines, '\n') .. '\n\n')
