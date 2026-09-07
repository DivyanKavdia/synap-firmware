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
const root=path.join(__dirname,'..');
function productionS3(){let s=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden,power])s=fn(s);return s}

test('final production S3 source matches audio, touch, low-power and OTA contract',()=>{
  const s3=productionS3();
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(s3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(s3,/ADPCM_BYTES_PER_FRAME == 404/);
  assert.match(s3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(s3,/MIN_REQUIRED_MTU = 32/);assert.match(s3,/MAX_AUDIO_PAYLOAD_BYTES = 500/);
  assert.match(s3,/MIC_START_ATTEMPTS=3[\s\S]*?attempt<=MIC_START_ATTEMPTS/);
  assert.match(s3,/microphoneValidated=startMicrophone\(\);\n  if \(microphoneValidated\) stopMicrophone\(\);/);
  assert.match(s3,/if \(microphoneReady\) \{ vTaskDelay\(pdMS_TO_TICKS\(90\)\); stopMicrophone\(\); \}/);
  assert.match(s3,/remote standby -> awake; microphone remains off until START/);
  assert.match(s3,/TOUCH_SLEEP_HOLD_MS = 5000/);assert.match(s3,/TOUCH_TAP_MIN_MS = 80/);assert.match(s3,/TOUCH_TAP_MAX_MS = 450/);
  assert.match(s3,/RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0/);
  assert.match(s3,/confirmTouchWakeTripleTap\(\)/);
  assert.match(s3,/tap 1\/3; waiting for taps 2 and 3/);
  assert.match(s3,/triple tap wake confirmed; continuing normal boot/);
  assert.doesNotMatch(s3,/wake detected; hold for 5 seconds to stay awake/);
  assert.match(s3,/double tap -> START/);assert.match(s3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(s3,/CMD_STANDBY = 0x03/);assert.match(s3,/CMD_WAKE = 0x04/);
  assert.match(s3,/POWER_STATE_WAKE_RECORD = 4/);
  assert.doesNotMatch(s3,/DeviceState::STANDBY/);
  assert.match(s3,/publishPowerEvent\(POWER_STATE_DEEP_SLEEP\)/);
  assert.match(s3,/batteryCritical\(\) && otaBusy\(\)[\s\S]*?otaSession\.fail\(Synap::BUSY\)/);
  assert.match(s3,/xTaskCreatePinnedToCore\(transmitterTask, "transmit", 8192/);
});

test('secondary target materialization preserves recording and wake-validation contract',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);assert.match(c3,/#define SYNAP_BATTERY_ADC_PIN 1/);assert.doesNotMatch(c3,/GPIO8/);
  assert.match(c3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(c3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(c3,/MIN_REQUIRED_MTU = 32/);
  assert.match(c3,/confirmTouchWakeTripleTap\(\)/);assert.match(c3,/triple tap wake confirmed; continuing normal boot/);
  assert.match(c3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(c3,/xTaskCreate\(transmitterTask, "transmit", 8192/);assert.doesNotMatch(c3,/xTaskCreatePinnedToCore/);
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup/);assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});

test('release workflow still compiles the exact final power-controls source',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  assert.match(workflow,/patch-production-hardening\.cjs/);assert.match(workflow,/patch-power-controls-v2\.cjs/);
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,2);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
});
