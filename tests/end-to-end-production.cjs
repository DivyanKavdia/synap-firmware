'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
function productionS3(){return fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8')}

test('final production S3 source matches audio, touch, low-power and OTA contract',()=>{
  const s3=productionS3();
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(s3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(s3,/ADPCM_BYTES_PER_FRAME == 404/);
  assert.match(s3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(s3,/MIN_REQUIRED_MTU = 32/);assert.match(s3,/MAX_AUDIO_PAYLOAD_BYTES = 500/);
  assert.match(s3,/MIC_START_ATTEMPTS=3[\s\S]*?attempt<=MIC_START_ATTEMPTS/);
  assert.match(s3,/microphoneValidated=startMicrophone\(\);\n  if \(microphoneValidated\) stopMicrophone\(\);/);
  assert.match(s3,/void stopStreaming\(ErrorCode reason\)[\s\S]*?#if USE_REAL_I2S_MIC\s*stopMicrophone\(\);/);
  assert.match(s3,/remote standby -> awake; microphone remains off until START/);
  assert.match(s3,/TOUCH_TAP_MIN_MS = 60/);assert.match(s3,/TOUCH_TAP_MAX_MS = 500/);
  assert.match(s3,/TOUCH_DOUBLE_TAP_GAP_MS = 550/);assert.match(s3,/TOUCH_STATE_LOCKOUT_MS = 250/);
  assert.match(s3,/RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0/);
  assert.match(s3,/confirmTouchWakeGesture\(\)/);
  assert.match(s3,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(s3,/long-press wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(s3,/long press -> DEEP SLEEP/);
  assert.match(s3,/enterDeepSleep\("touch-hold"\)/);
  assert.match(s3,/enterDeepSleep\("touch-hold-after-stop"\)/);
  assert.match(s3,/held>=TOUCH_SLEEP_HOLD_MS/);
  assert.match(s3,/double tap -> START/);assert.match(s3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(s3,/CMD_STANDBY = 0x03/);assert.match(s3,/CMD_WAKE = 0x04/);
  assert.match(s3,/POWER_STATE_AWAKE = 1/);
  assert.doesNotMatch(s3,/DeviceState::STANDBY/);
  assert.match(s3,/publishPowerEvent\(POWER_STATE_DEEP_SLEEP\)/);
  assert.match(s3,/batteryCritical\(\) && otaBusy\(\)[\s\S]*?otaSession\.fail\(Synap::BUSY\)/);
  assert.match(s3,/xTaskCreatePinnedToCore\(transmitterTask, "transmit", 8192/);
});

test('secondary C3 target retains shared gestures and its own pins and tasks',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);assert.match(c3,/#define SYNAP_BATTERY_ADC_PIN 1/);assert.doesNotMatch(c3,/GPIO8/);
  assert.match(c3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(c3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(c3,/MIN_REQUIRED_MTU = 32/);
  assert.match(c3,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(c3,/TOUCH_SLEEP_HOLD_MS = 4000/);
  assert.match(c3,/TOUCH_DOUBLE_TAP_GAP_MS = 550/);
  assert.match(c3,/long-press wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(c3,/long press -> DEEP SLEEP/);
  assert.match(c3,/double tap -> START/);
  assert.match(c3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(c3,/double tap -> SD audio START/);
  assert.match(c3,/double tap -> SD audio STOP/);
  assert.match(c3,/double tap ignored: SD unavailable/);
  assert.match(c3,/odysseySdDetectionState\(\)!=1/);
  assert.match(c3,/ready\|=SYNAP_CAP_SDAUDIO/);
  assert.match(c3,/\[SD\] probe CS=%d SCK=%d MOSI=%d MISO=%d/);
  assert.match(c3,/ODYSSEY_SD_STARTUP_SETTLE_MS=3000u/);
  assert.match(c3,/ODYSSEY_SD_MOUNT_ATTEMPTS=3/);
  assert.match(c3,/healthy mount retained/);
  assert.match(c3,/Normal PWA reload\/reconnect is observational only: never remount here/);
  assert.match(c3,/Explicit Settings recovery is the only connected-path remount/);
  const sdBoot=c3.indexOf('odysseyInitializeSdCardBeforeBle();');
  const bleInit=c3.indexOf('initializeBLE();',sdBoot);
  const tasks=c3.indexOf('xTaskCreate(controlTask, "control"',bleInit);
  assert.ok(sdBoot>0 && bleInit>sdBoot && tasks>bleInit,'C3 SD must finish initialization before BLE advertising and runtime tasks');
  assert.doesNotMatch(c3,/odysseyScheduleSdCardDetection/);
  assert.match(c3,/enterDeepSleep\("touch-hold"\)/);
  assert.match(c3,/enterDeepSleep\("touch-hold-after-stop"\)/);
  assert.doesNotMatch(c3,/triple tap -> DEEP SLEEP/);
  assert.doesNotMatch(c3,/tap 1\/3; waiting for taps 2 and 3/);
  assert.match(c3,/xTaskCreate\(transmitterTask, "transmit", 8192/);assert.doesNotMatch(c3,/xTaskCreatePinnedToCore/);
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 1/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup/);assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});

test('release workflow compiles the shared complete production pipeline',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  assert.match(workflow,/cp synap_esp32s3\/synap_esp32s3\.ino prepared\/synap_esp32s3\/synap_esp32s3\.ino/);
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
});
test('C3 completes SD initialization before BLE and never remounts on normal PWA access',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  const sd=c3.indexOf('odysseyInitializeSdCardBeforeBle();');
  const ble=c3.indexOf('initializeBLE();',sd);
  assert(sd>0 && ble>sd,'C3 SD boot initialization must complete before BLE advertising');
  assert.match(c3,/delay\(ODYSSEY_SD_STARTUP_SETTLE_MS\)/);
  assert.match(c3,/for \(uint8_t attempt=1;attempt<=ODYSSEY_SD_MOUNT_ATTEMPTS;\+\+attempt\)/);
  const storage=c3.match(/static bool storageReady\(\) \{[\s\S]*?\n\}/)?.[0]||'';
  assert.doesNotMatch(storage,/odysseyDetectSdCard|odysseyRecoverSdCard/);
  const toggle=c3.match(/void odysseyToggleRecording\(\) \{[\s\S]*?\n\}/)?.[0]||'';
  assert.doesNotMatch(toggle,/odysseyDetectSdCard|odysseyRecoverSdCard/);
});
