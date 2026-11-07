-- Minimal JSON encoder: sorted object keys so the same data always gives the same file.
-- Tables are objects unless marked with json.array() or they have a [1] entry.

local M = {}

local ARRAY = {}   -- metatable that marks a table as a JSON array (needed for empty arrays)

-- Mark t (or a new table) as an array, so an empty one encodes as [] rather than {}.
function M.array(t)
  return setmetatable(t or {}, ARRAY)
end

local ESC = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

-- A JSON string: quotes, backslashes and control characters escaped.
local function str(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    return ESC[c] or string.format('\\u%04x', c:byte())
  end) .. '"'
end

-- A JSON number: whole numbers without ".0", others as short as possible while reading back
-- exactly; NaN and infinity have no JSON form and become null.
local function num(x)
  if x ~= x or x == math.huge or x == -math.huge then return 'null' end
  if math.type(x) == 'integer' then return string.format('%d', x) end
  if x == math.floor(x) and math.abs(x) < 2 ^ 53 then return string.format('%d', math.floor(x)) end
  local s = string.format('%.14g', x)
  if tonumber(s) ~= x then s = string.format('%.17g', x) end
  return s
end

local function is_array(t)
  return getmetatable(t) == ARRAY or t[1] ~= nil
end

-- Appends the pieces of v's encoding to out (joined once at the end: no repeated string copies).
local function encode(v, indent, depth, out)
  local tv = type(v)
  if tv == 'nil' then out[#out + 1] = 'null'
  elseif tv == 'boolean' then out[#out + 1] = tostring(v)
  elseif tv == 'number' then out[#out + 1] = num(v)
  elseif tv == 'string' then out[#out + 1] = str(v)
  elseif tv == 'table' then
    local pad = indent and ('\n' .. string.rep(indent, depth + 1)) or ''
    local close = indent and ('\n' .. string.rep(indent, depth)) or ''
    local sep = indent and ': ' or ':'
    if is_array(v) then
      if #v == 0 then out[#out + 1] = '[]' return end
      out[#out + 1] = '['
      for i = 1, #v do
        out[#out + 1] = (i > 1 and ',' or '') .. pad
        encode(v[i], indent, depth + 1, out)
      end
      out[#out + 1] = close .. ']'
    else
      local keys = {}
      for k in pairs(v) do keys[#keys + 1] = tostring(k) end
      table.sort(keys)
      if #keys == 0 then out[#out + 1] = '{}' return end
      out[#out + 1] = '{'
      for i, k in ipairs(keys) do
        out[#out + 1] = (i > 1 and ',' or '') .. pad .. str(k) .. sep
        encode(v[k], indent, depth + 1, out)
      end
      out[#out + 1] = close .. '}'
    end
  else
    error('cannot encode ' .. tv .. ' as JSON')
  end
end

-- Encode v; indent (e.g. '  ') gives one value per line, nil gives the compact form.
function M.encode(v, indent)
  local out = {}
  encode(v, indent, 0, out)
  return table.concat(out)
end

return M
