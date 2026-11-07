// Runs hosts/reaper/tests/test_*.lua under fengari (Lua 5.3 in JavaScript) with a fake REAPER API.
// REAPER itself runs Lua 5.4; the modules avoid 5.4-only syntax so both agree.
// fengari has no io.open, so a small one backed by Node's fs is provided (prelude.lua).

'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { lua, lauxlib, lualib, to_luastring, to_jsstring } = require('fengari');

const here = __dirname;
const lib = path.join(here, '..', 'Scripts', 'SessionScope', 'lib');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sessionscope-lua-'));

function register(L, name, fn) {
  lua.lua_pushcfunction(L, fn);
  lua.lua_setglobal(L, to_luastring(name));
}

function newState() {
  const L = lauxlib.luaL_newstate();
  lualib.luaL_openlibs(L);
  register(L, '__fs_write', (L) => {
    const p = to_jsstring(lauxlib.luaL_checkstring(L, 1));
    const data = Buffer.from(lauxlib.luaL_checkstring(L, 2));
    if (lua.lua_toboolean(L, 3)) fs.appendFileSync(p, data); else fs.writeFileSync(p, data);
    return 0;
  });
  register(L, '__fs_read', (L) => {
    const p = to_jsstring(lauxlib.luaL_checkstring(L, 1));
    if (!fs.existsSync(p)) { lua.lua_pushnil(L); return 1; }
    lua.lua_pushstring(L, new Uint8Array(fs.readFileSync(p)));
    return 1;
  });
  register(L, '__mkdir', (L) => {
    fs.mkdirSync(to_jsstring(lauxlib.luaL_checkstring(L, 1)), { recursive: true });
    return 0;
  });
  lua.lua_pushstring(L, to_luastring(tmp.replace(/\\/g, '/')));
  lua.lua_setglobal(L, to_luastring('__tmpdir'));
  lua.lua_pushstring(L, to_luastring(path.join(here, '..', '..', '..').replace(/\\/g, '/')));
  lua.lua_setglobal(L, to_luastring('__repo'));
  const setup = `package.path = ${JSON.stringify(lib.replace(/\\/g, '/') + '/?.lua;' + here.replace(/\\/g, '/') + '/?.lua')}`;
  if (lauxlib.luaL_dostring(L, to_luastring(setup)) !== lua.LUA_OK) throw new Error(to_jsstring(lua.lua_tostring(L, -1)));
  return L;
}

let failed = 0, passed = 0;
const files = fs.readdirSync(here).filter((f) => /^test_.*\.lua$/.test(f)).sort();
for (const f of files) {
  const L = newState();
  const status = lauxlib.luaL_dofile(L, to_luastring(path.join(here, 'prelude.lua'))) ||
    lauxlib.luaL_dofile(L, to_luastring(path.join(here, f))) ||
    lauxlib.luaL_dostring(L, to_luastring('return __report()'));
  if (status !== lua.LUA_OK) {
    failed++;
    console.log(`FAIL ${f}: ${to_jsstring(lua.lua_tostring(L, -1))}`);
    continue;
  }
  const res = JSON.parse(to_jsstring(lua.lua_tostring(L, -1)));
  passed += res.passed;
  failed += res.failed.length;
  for (const msg of res.failed) console.log(`FAIL ${f}: ${msg}`);
  console.log(`${res.failed.length ? 'FAIL' : 'ok  '} ${f} (${res.passed} passed)`);
}
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
