'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch}=require('../tools/patch-runtime-fixes.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {TARGETS,PRIMARY_TARGET}=require('../tools/targets.cjs');
const root=path.join(__dirname,'..');

function prepared(){
  return patch(prepare(fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8')));
}

test('target catalog keeps S3 as backward-compatible primary and adds C3',()=>{
  assert.equal(PRIMARY_TARGET,'esp32s3-fh4r2-qspi-4m');
  assert.equal(TARGETS[PRIMARY_TARGET].chip,9);
  assert.equal(TARGETS['esp32c3-supermini-4m'].chip,5);
  assert.equal(TARGETS['esp32c3-supermini-4m'].psramBytes,0);
  assert.equal(TARGETS['esp32c3-supermini-4m'].manifestPath,'targets/esp32c3-supermini-4m/latest.json');
});

test('C3 program is generated from the same prepared production source with target-safe differences only',()=>{
  const s3=prepared(),c3=materialize(s3,'esp32c3-supermini-4m');
  assert.match(s3,/SYNAP-FW:esp32s3-fh4r2-qspi-4m:0\.0\.1:/);
  assert.match(c3,/SYNAP-FW:esp32c3-supermini-4m:0\.0\.1:/);
  assert.match(c3,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.doesNotMatch(c3,/SYNAP-ESP32S3-OTA-ID-V3/);
  assert.match(c3,/p\[21\]!=5 \|\| p\[22\]!=0/);
  assert.match(c3,/constexpr uint8_t RGB_LED_PIN = 8;/);
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/#define SYNAP_BATTERY_ADC_PIN 1/);
  assert.match(c3,/xTaskCreate\(controlTask/);
  assert.doesNotMatch(c3,/xTaskCreatePinnedToCore/);
  assert.match(c3,/I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6/);
  assert.match(c3,/900000u/,'mobile OTA resume behavior is shared');
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/,'battery monitor remains disabled until hardware is audited');
});
