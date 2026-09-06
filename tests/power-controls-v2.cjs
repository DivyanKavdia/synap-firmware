'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
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

test('deep sleep requires a continuous 5s hold; connected idle uses double tap to record',()=>{
  const s3=productionS3();
  assert.match(s3,/wake detected; hold for 5 seconds to stay awake/);
  assert.match(s3,/5 second wake hold confirmed/);
  assert.match(s3,/wake hold too short; returning to deep sleep/);
  assert.match(s3,/millis\(\)-started\)>=TOUCH_SLEEP_HOLD_MS/);
  assert.doesNotMatch(s3,/DEEP_SLEEP_SECOND_TAP_WINDOW_MS/);
  assert.doesNotMatch(s3,/deep-sleep double tap/);
  assert.match(s3,/double tap -> START/);
  assert.match(s3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(s3,/held>=TOUCH_SLEEP_HOLD_MS/);
});

test('standby is internal and remains protocol-v2 CONNECTED_IDLE',()=>{
  const s3=productionS3();
  assert.match(s3,/CMD_STANDBY = 0x03/);
  assert.match(s3,/CMD_WAKE = 0x04/);
  assert.match(s3,/remoteStandby = false/);
  assert.match(s3,/POWER_STATE_WAKE_RECORD = 4/);
  assert.match(s3,/standby is still CONNECTED_IDLE/);
  assert.match(s3,/remote standby; BLE available, mic\/I2S off/);
  assert.doesNotMatch(s3,/DeviceState::STANDBY/,'standby must not leak a new status state to the current PWA');
});

test('idle and normal STOP power down the microphone while START keeps hardened retries',()=>{
  const s3=productionS3();
  assert.match(s3,/MIC_START_ATTEMPTS=3/);
  assert.match(s3,/microphoneRecoveryUsed=false/);
  assert.match(s3,/if \(microphoneReady\) \{ vTaskDelay\(pdMS_TO_TICKS\(90\)\); stopMicrophone\(\); \}/);
  assert.match(s3,/microphoneValidated=startMicrophone\(\);\n  if \(microphoneValidated\) stopMicrophone\(\);/);
  assert.match(s3,/remote standby -> awake; microphone remains off until START/);
  assert.match(s3,/startStreaming\(version\)/);
});

test('recording double tap stops into standby; long hold still reaches deep sleep',()=>{
  const s3=productionS3();
  assert.match(s3,/standbyAfterStop=true/);
  assert.match(s3,/enterRemoteStandby\(\)/);
  assert.match(s3,/deepSleepAfterStop=true/);
  assert.match(s3,/enterDeepSleep\("touch-hold-after-stop"\)/);
});

test('C3 gets the same power and gesture contract with its target-safe wake API',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/CMD_STANDBY = 0x03/);
  assert.match(c3,/wake detected; hold for 5 seconds to stay awake/);
  assert.match(c3,/5 second wake hold confirmed/);
  assert.match(c3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});
