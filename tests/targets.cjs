'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:events}=require('../tools/patch-event-channel.cjs');
const {patch:battery}=require('../tools/patch-battery-v2.cjs');
const {patch:audio}=require('../tools/patch-audio-reliability.cjs');
const {patch:codec}=require('../tools/patch-audio-codec-v3.cjs');
const {patch:touch}=require('../tools/patch-touch-reliability.cjs');
const {patch:harden}=require('../tools/patch-production-hardening.cjs');
const {patch:power}=require('../tools/patch-power-controls-v2.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {TARGETS,PRIMARY_TARGET}=require('../tools/targets.cjs');
const root=path.join(__dirname,'..');
function prepared(){let source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden,power])source=fn(source);return source}

test('target catalog keeps S3 SuperMini as primary',()=>{
  assert.equal(PRIMARY_TARGET,'esp32s3-fh4r2-qspi-4m');
  assert.equal(TARGETS[PRIMARY_TARGET].board,'ESP32-S3 SuperMini');
  assert.equal(TARGETS[PRIMARY_TARGET].chip,9);
  assert.equal(TARGETS[PRIMARY_TARGET].flashBytes,4194304);
  assert.equal(TARGETS[PRIMARY_TARGET].psramBytes,2097152);
});

test('secondary generated target preserves the production interaction contract',()=>{
  const s3=prepared(),c3=materialize(s3,'esp32c3-supermini-4m');
  assert.match(s3,/SYNAP-FW:esp32s3-fh4r2-qspi-4m:1\.0\.0:/);
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(s3,/confirmTouchWakeTripleTap\(\)/);
  assert.match(s3,/triple tap wake confirmed; continuing normal boot/);
  assert.match(c3,/SYNAP-FW:esp32c3-supermini-4m:1\.0\.0:/);
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup\(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH\)/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
  assert.match(c3,/confirmTouchWakeTripleTap\(\)/);
  assert.match(c3,/triple tap wake confirmed; continuing normal boot/);
  assert.match(c3,/double tap -> START/);
  assert.match(c3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(c3,/900000u/);
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/);
});
