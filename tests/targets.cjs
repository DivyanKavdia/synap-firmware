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
const {materialize}=require('../tools/materialize-target.cjs');
const {TARGETS,PRIMARY_TARGET}=require('../tools/targets.cjs');
const root=path.join(__dirname,'..');
function prepared(){let source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden])source=fn(source);return source}

test('target catalog keeps S3 as backward-compatible primary and adds C3',()=>{
  assert.equal(PRIMARY_TARGET,'esp32s3-fh4r2-qspi-4m');
  assert.equal(TARGETS[PRIMARY_TARGET].chip,9);
  assert.equal(TARGETS['esp32c3-supermini-4m'].chip,5);
  assert.equal(TARGETS['esp32c3-supermini-4m'].psramBytes,0);
  assert.equal(TARGETS['esp32c3-supermini-4m'].manifestPath,'targets/esp32c3-supermini-4m/latest.json');
});

test('C3 program is generated from the exact final production source with target-safe differences only',()=>{
  const s3=prepared(),c3=materialize(s3,'esp32c3-supermini-4m');
  assert.match(s3,/SYNAP-FW:esp32s3-fh4r2-qspi-4m:1\.0\.0:/);
  assert.match(c3,/SYNAP-FW:esp32c3-supermini-4m:1\.0\.0:/);
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/SYNAP-ESP32C3-OTA-ID-V3/);
  assert.doesNotMatch(c3,/SYNAP-ESP32S3-OTA-ID-V3/);
  assert.match(c3,/p\[21\]!=5 \|\| p\[22\]!=0/);
  assert.match(c3,/constexpr uint8_t RGB_LED_PIN = 8;/);
  assert.match(c3,/#define SYNAP_BATTERY_ADC_PIN 1/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup\(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH\)/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
  assert.match(c3,/xTaskCreate\(controlTask/);
  assert.doesNotMatch(c3,/xTaskCreatePinnedToCore/);
  assert.match(c3,/I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6/);
  assert.match(c3,/AUDIO_PROTOCOL_VERSION = 3/);
  assert.match(c3,/MIN_CHUNKS_PER_FRAME = 1/);
  assert.match(c3,/900000u/,'mobile OTA resume behavior is shared');
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/,'battery monitor remains disabled until hardware is audited');
  assert.doesNotMatch(c3,/GPIO8/,'generated C3 source must not retain S3 battery-pin comments');
});
