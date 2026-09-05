'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:touch}=require('../tools/patch-touch-reliability.cjs');
const root=path.join(__dirname,'..');

test('TTP223 rejects accidental brushes and preserves deliberate gestures',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const prepared=touch(runtime(prepare(source)));
  assert.match(prepared,/pinMode\(TOUCH_INPUT_PIN, INPUT\);/);
  assert.doesNotMatch(prepared,/pinMode\(TOUCH_INPUT_PIN, INPUT_PULLDOWN\);/);
  assert.match(prepared,/TOUCH_START_HOLD_MS = 550/);
  assert.match(prepared,/TOUCH_START_MAX_MS = 1400/);
  assert.match(prepared,/TOUCH_STOP_MIN_MS = 140/);
  assert.match(prepared,/TOUCH_STOP_MAX_MS = 850/);
  assert.match(prepared,/TOUCH_REARM_MS = 650/);
  assert.match(prepared,/TOUCH_STATE_LOCKOUT_MS = 900/);
  assert.match(prepared,/wasIdle && held>=TOUCH_START_HOLD_MS && held<=TOUCH_START_MAX_MS/);
  assert.match(prepared,/!wasIdle && held>=TOUCH_STOP_MIN_MS && held<=TOUCH_STOP_MAX_MS/);
  assert.match(prepared,/\[TOUCH\] deliberate start hold %ums -> START/);
  assert.match(prepared,/\[TOUCH\] deliberate stop tap %ums -> STOP/);
  assert.match(prepared,/\[TOUCH\] ignored gesture %ums idle=%u/);
  assert.match(prepared,/\[TOUCH\] ignored during state lockout/);
  assert.match(prepared,/touchLongSent=true;[\s\S]*publishRememberEvent\(\)/);
  assert.match(prepared,/held>=TOUCH_SLEEP_HOLD_MS/);
});
