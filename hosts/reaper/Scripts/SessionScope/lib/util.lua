-- Small helpers shared by the scripts: paths, timestamps and CSV writing.

local M = {}

-- The path separator a path already uses (REAPER on Windows hands out backslashes).
function M.sep(path)
  return path:find('\\', 1, true) and '\\' or '/'
end

-- a/b, without doubling a separator a already ends with.
function M.join(a, b)
  if a:sub(-1) == '/' or a:sub(-1) == '\\' then return a .. b end
  return a .. M.sep(a) .. b
end

-- The last part of a path: the file name.
function M.basename(path)
  if not path then return nil end
  return path:match('([^/\\]+)$') or path
end

-- Local date and time for folder names. Sorts in time order, so the newest folder is the last.
function M.stamp()
  return os.date('%Y%m%d-%H%M%S')
end

-- One CSV cell. Numbers keep microsecond precision for any plausible time_precise() value;
-- nil and NaN stay empty (unknown, never zero); text with commas or quotes is quoted.
local function cell(v)
  if v == nil then return '' end
  if type(v) == 'boolean' then return v and '1' or '0' end
  if type(v) == 'number' then
    if v ~= v then return '' end                                  -- NaN
    if math.type(v) == 'integer' or (v == math.floor(v) and math.abs(v) < 2 ^ 53) then
      return string.format('%d', math.floor(v))                   -- whole numbers without ".0"
    end
    return string.format('%.15g', v)
  end
  local s = tostring(v)
  if s:find('[,"\n\r]') then s = '"' .. s:gsub('"', '""') .. '"' end
  return s
end

function M.csv_row(values)
  local out = {}
  for i = 1, #values do out[i] = cell(values[i]) end
  return table.concat(out, ',') .. '\n'
end

-- Buffered CSV writer: rows collect in memory and go to disk when flush() is called (the
-- recorder does that once a second), so a defer tick never waits on the disk.
function M.csv(path, header)
  local f = assert(io.open(path, 'wb'))
  f:write(table.concat(header, ',') .. '\n')
  local w = { f = f, buf = {}, rows = 0 }
  function w.row(values)
    w.buf[#w.buf + 1] = M.csv_row(values)
    w.rows = w.rows + 1
  end
  function w.flush()
    if #w.buf > 0 then
      f:write(table.concat(w.buf))
      w.buf = {}
      f:flush()
    end
  end
  function w.close()
    w.flush()
    f:close()
  end
  return w
end

-- Replace a whole file's contents (meta.json, inventory, manifest).
function M.write_file(path, text)
  local f = assert(io.open(path, 'wb'))
  f:write(text)
  f:close()
end

return M
