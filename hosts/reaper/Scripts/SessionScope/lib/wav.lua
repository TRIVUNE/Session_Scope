-- Author-generated test audio, so fixtures never depend on anyone's music (blueprint appendix C).

local M = {}

-- Mono 16-bit sine. A whole number of cycles fits any whole-second length, so it loops cleanly.
function M.sine(path, srate, seconds, freq, amp)
  local n = math.floor(srate * seconds + 0.5)
  local parts = {}
  for i = 0, n - 1 do
    parts[i + 1] = string.pack('<i2', math.floor(amp * 32767 * math.sin(2 * math.pi * freq * i / srate) + 0.5))
  end
  local data = table.concat(parts)
  local f = assert(io.open(path, 'wb'))
  f:write('RIFF', string.pack('<I4', 36 + #data), 'WAVE',
    'fmt ', string.pack('<I4I2I2I4I4I2I2', 16, 1, 1, srate, srate * 2, 2, 16),
    'data', string.pack('<I4', #data), data)
  f:close()
  return n
end

return M
