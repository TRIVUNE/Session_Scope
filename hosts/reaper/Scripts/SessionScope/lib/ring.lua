-- Reader for the sensor's gmem ring (writer: hosts/reaper/Effects/SessionScope/SessionScope Sensor.jsfx).
-- The writer stores a record's fields, then publishes it by bumping the record count. The reader
-- copies records out, then checks that the writer has not lapped it during the copy; lapped or torn
-- records are dropped and counted, never guessed.

local M = {}

-- Must match the constants in the JSFX (a test parses the JSFX and compares).
M.MAGIC = 53530001               -- "a SessionScope ring, layout 1, lives here"
M.LAYOUT = 1
M.HDR = 16                       -- header slots before the first record
M.SEGMENT = 'SessionScope_v0'    -- the gmem segment name (options:gmem= in the JSFX)

-- Header slot numbers.
local H = { magic = 0, layout = 1, count = 2, cap = 3, rec = 4, owner = 5, owner_t = 6, other = 7, other_t = 8, version = 9 }
M.H = H

-- Record fields in slot order; record n starts at HDR + (n % cap) * #FIELDS.
M.FIELDS = { 't', 'frames', 'srate', 'play_state', 'play_pos', 'qn', 'tempo', 'ts_num', 'ts_den', 'self_us', 'seq', 'instance' }

-- read(index) returns the gmem value at index (reaper.gmem_read once attached to M.SEGMENT).
-- `next` is the sequence number of the next record to read; `dropped` and `resets` are counters.
function M.new(read)
  return { read = read, next = nil, dropped = 0, resets = 0 }
end

-- Header facts, or nil when no sensor has written the expected layout yet.
function M.header(r)
  local read = r.read
  if read(H.magic) ~= M.MAGIC or read(H.layout) ~= M.LAYOUT then return nil end
  return {
    count = read(H.count), cap = read(H.cap), rec = read(H.rec), version = read(H.version),
    owner = read(H.owner), owner_t = read(H.owner_t), other = read(H.other), other_t = read(H.other_t),
  }
end

-- Records published since the previous call (at most max), oldest first.
-- The first call only sets the starting point: history from before the recorder started is skipped.
function M.drain(r, max)
  local hd = M.header(r)
  if not hd then return {}, nil end
  local read, count, cap, rec = r.read, hd.count, hd.cap, hd.rec
  if r.next == nil then
    r.next = count
    return {}, hd
  end
  if count < r.next then           -- the sensor re-initialised the ring (reload, layout change)
    r.resets = r.resets + 1
    r.next = 0
  end
  -- While the count is n + cap the writer may already be overwriting record n's slot, so only the
  -- newest cap - 1 records can be read safely. Falling further behind loses the rest.
  if count - r.next > cap - 1 then
    r.dropped = r.dropped + (count - (cap - 1) - r.next)
    r.next = count - (cap - 1)
  end
  local stop = count
  if max and stop - r.next > max then stop = r.next + max end
  local out, first = {}, r.next
  for n = first, stop - 1 do
    local base = M.HDR + (n % cap) * rec
    local x = {}
    for i = 1, #M.FIELDS do x[M.FIELDS[i]] = read(base + i - 1) end
    out[#out + 1] = x
  end
  -- Record n is safe only if the writer had not started on record n + cap when the copy ended.
  local after = read(H.count)
  local kept = {}
  for i, x in ipairs(out) do
    local n = first + i - 1
    if after >= n + cap or x.seq ~= n then
      r.dropped = r.dropped + 1
    else
      kept[#kept + 1] = x
    end
  end
  r.next = stop
  return kept, hd
end

return M
