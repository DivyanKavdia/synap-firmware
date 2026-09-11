'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const {TARGETS,PRIMARY_TARGET}=require('../tools/targets.cjs');
const root=path.join(__dirname,'..');
function prepared(){return fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8')}

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
  assert.match(s3,/triple tap wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(c3,/SYNAP-FW:esp32c3-supermini-4m:1\.0\.0:/);
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup\(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH\)/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
  assert.match(c3,/confirmTouchWakeTripleTap\(\)/);
  assert.match(c3,/C3 long-press wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(c3,/double tap -> START/);
  assert.match(c3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(c3,/900000u/);
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/);
});
