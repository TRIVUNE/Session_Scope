local json = require('json')

test('objects have sorted keys', function()
  eq(json.encode({ b = 1, a = 'x', c = true }), '{"a":"x","b":1,"c":true}')
end)

test('arrays, empty arrays and empty objects', function()
  eq(json.encode({ 1, 2, 3 }), '[1,2,3]')
  eq(json.encode(json.array()), '[]')
  eq(json.encode({}), '{}')
  eq(json.encode({ list = json.array({ { x = 1 } }) }), '{"list":[{"x":1}]}')
end)

test('numbers: integers stay integers, floats round-trip, NaN becomes null', function()
  eq(json.encode(48000.0), '48000')
  eq(json.encode(0.1), '0.1')
  eq(tonumber(json.encode(1 / 3)), 1 / 3)
  eq(json.encode(0 / 0), 'null')
  eq(json.encode(math.huge), 'null')
end)

test('strings are escaped', function()
  eq(json.encode('a"b\\c\nd\1'), '"a\\"b\\\\c\\nd\\u0001"')
end)

test('indentation', function()
  eq(json.encode({ a = json.array({ 1 }) }, '  '), '{\n  "a": [\n    1\n  ]\n}')
end)
