'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:touch}=require('../tools/patch-touch-reliability.cjs');
const root=path.join(__dirname,'..');

test('TTP223 uses direct push-pull input and deliberate tap toggle',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const prepared=touch(runtime(prepare(source)));
  assert.match(prepared,/pinMode\(TOUCH_INPUT_PIN, INPUT\);/);
  assert.doesNotMatch(prepared,/pinMode\(TOUCH_INPUT_PIN, INPUT_PULLDOWN\);/);
  assert.match(prepared,/TOUCH_MIN_TAP_MS = 80/);
  assert.match(prepared,/TOUCH_REARM_MS = 220/);
  assert.match(prepared,/streamingEnabled\.load\(\)\?CMD_STOP:CMD_START/);
  assert.match(prepared,/\[TOUCH\] tap %ums -> %s/);
  assert.match(prepared,/\[TOUCH\] ignored short pulse %ums/);
  assert.match(prepared,/touchLongSent=true;[\s\S]*publishRememberEvent\(\)/);
});
