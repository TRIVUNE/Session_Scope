-- Test helpers, plus the io.open that fengari lacks (backed by Node's fs in run.js).

io = io or {}
function io.open(path, mode)
  mode = mode or 'r'
  if mode:sub(1, 1) == 'r' then
    local data = __fs_read(path)
    if not data then return nil, path .. ': No such file or directory' end
    return {
      read = function(_, fmt)
        assert(fmt == 'a' or fmt == '*a', 'only read("a") is supported')
        local s = data
        data = ''
        return s
      end,
      close = function() end,
    }
  end
  if mode:sub(1, 1) ~= 'a' then __fs_write(path, '', false) end
  local f, buf = {}, {}
  function f:write(...)
    for i = 1, select('#', ...) do buf[#buf + 1] = tostring((select(i, ...))) end
    return self
  end
  function f:flush()
    if #buf > 0 then __fs_write(path, table.concat(buf), true) buf = {} end
  end
  function f:close() self:flush() end
  return f
end

function read_file(path)
  local f = assert(io.open(path, 'rb'))
  local s = f:read('a')
  f:close()
  return s
end

local passed, failed = 0, {}

local function show(v)
  if type(v) == 'string' then return string.format('%q', v) end
  return tostring(v)
end

function test(name, fn)
  local ok, err = pcall(fn)
  if ok then passed = passed + 1 else failed[#failed + 1] = name .. ': ' .. tostring(err) end
end

function eq(actual, expected, what)
  if actual ~= expected then
    error(string.format('%sexpected %s, got %s', what and (what .. ': ') or '', show(expected), show(actual)), 2)
  end
end

function near(actual, expected, tol, what)
  if type(actual) ~= 'number' or math.abs(actual - expected) > (tol or 1e-9) then
    error(string.format('%sexpected %s +/- %s, got %s', what and (what .. ': ') or '', show(expected), show(tol), show(actual)), 2)
  end
end

function truthy(v, what)
  if not v then error((what or 'expected a true value') .. ', got ' .. show(v), 2) end
end

function __report()
  local json = require('json')
  return json.encode({ passed = passed, failed = json.array(failed) })
end
