local util = require('util')

test('csv cells: integers, microsecond floats, booleans, empty, quoting', function()
  eq(util.csv_row({ 48000.0, 1234567.123456789, true, false, nil, 'a,b', 'say "hi"' }),
    '48000,1234567.12345679,1,0,,"a,b","say ""hi"""\n')
  eq(util.csv_row({ 0 / 0 }), '\n')
end)

test('paths', function()
  eq(util.join('C:\\REAPER', 'x'), 'C:\\REAPER\\x')
  eq(util.join('/home/r/', 'x'), '/home/r/x')
  eq(util.basename('C:\\Program Files\\VST3\\Thing.vst3'), 'Thing.vst3')
  eq(util.basename(nil), nil)
end)

test('buffered csv writer', function()
  local path = __tmpdir .. '/w.csv'
  local w = util.csv(path, { 'a', 'b' })
  w.row({ 1, 2 })
  w.flush()
  eq(read_file(path), 'a,b\n1,2\n', 'after flush')
  w.row({ 3, 4 })
  w.close()
  eq(read_file(path), 'a,b\n1,2\n3,4\n')
  eq(w.rows, 2)
end)
