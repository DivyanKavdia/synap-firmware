'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
function productionS3(){return fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8')}

test('deep sleep requires stable release and a confirmed hold before BLE boot',()=>{
  const s3=productionS3();
  assert.match(s3,/initialReleaseAt=millis\(\)/);
  assert.match(s3,/millis\(\)-initialReleaseAt\)<300u/);
  assert.match(s3,/RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0/);
  assert.match(s3,/synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER/);
  assert.match(s3,/confirmTouchWakeGesture\(\)/);
  assert.match(s3,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(s3,/long-press wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(s3,/if \(!confirmTouchWakeGesture\(\)\) return;/);
  assert.match(s3,/if \(bootSleepWasLocked\) delay\(20\)/);
  assert.match(s3,/esp_sleep_enable_ext0_wakeup\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\),1\)/);
  assert.match(s3,/rtc_gpio_deinit\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\)\)/);
  assert.doesNotMatch(s3,/esp_sleep_enable_ext1_wakeup/);
});

test('awake hold sleeps while the second tap immediately controls recording',()=>{
  const s3=productionS3();
  assert.match(s3,/TOUCH_DOUBLE_TAP_GAP_MS = 550/);
  assert.match(s3,/TOUCH_SLEEP_HOLD_MS = 4000/);
  assert.doesNotMatch(s3,/pendingDoubleAt|AWAKE_TRIPLE/);
  assert.match(s3,/long press -> DEEP SLEEP/);
  assert.match(s3,/enterDeepSleep\("touch-hold"\)/);
  assert.match(s3,/enterDeepSleep\("touch-hold-after-stop"\)/);
  assert.match(s3,/double tap -> START/);
  assert.match(s3,/double tap -> STOP \+ POWER SAVER/);
  assert.match(s3,/held>=TOUCH_SLEEP_HOLD_MS/);
});

test('standby is internal and remains protocol-v2 CONNECTED_IDLE',()=>{
  const s3=productionS3();
  assert.match(s3,/CMD_STANDBY = 0x03/);
  assert.match(s3,/CMD_WAKE = 0x04/);
  assert.match(s3,/remoteStandby = false/);
  assert.match(s3,/POWER_STATE_AWAKE = 1/);
  assert.match(s3,/Standby remains CONNECTED_IDLE on protocol v2/);
  assert.match(s3,/remote standby; BLE available, mic\/I2S off/);
  assert.doesNotMatch(s3,/DeviceState::STANDBY/);
});

test('idle and normal STOP power down the microphone while START keeps hardened retries',()=>{
  const s3=productionS3();
  assert.match(s3,/MIC_START_ATTEMPTS=3/);
  assert.match(s3,/microphoneRecoveryUsed=false/);
  assert.match(s3,/void stopStreaming\(ErrorCode reason\)[\s\S]*?#if USE_REAL_I2S_MIC\s*stopMicrophone\(\);/);
  assert.match(s3,/microphoneValidated=startMicrophone\(\);\n  if \(microphoneValidated\) stopMicrophone\(\);/);
  assert.match(s3,/remote standby -> awake; microphone remains off until START/);
});

test('secondary target materialization preserves C3 hold validation and GPIO wake',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/confirmTouchWakeGesture\(\)/);
  assert.match(c3,/long-press wake confirmed; sleep lock cleared; continuing normal boot/);
  assert.match(c3,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(c3,/TOUCH_SLEEP_HOLD_MS = 4000/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup\(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH\)/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});
