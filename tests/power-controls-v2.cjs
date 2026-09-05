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
function productionS3(){
  let s=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden,power])s=fn(s);
  return s;
}

test('production touch model is single-wake, double-toggle and five-second deep sleep',()=>{
  const s3=productionS3();
  assert.match(s3,/TOUCH_TAP_MIN_MS = 80/);
  assert.match(s3,/TOUCH_TAP_MAX_MS = 450/);
  assert.match(s3,/double tap -> START/);
  assert.match(s3,/double tap -> STOP/);
  assert.match(s3,/single touch wake/);
  assert.match(s3,/held>=TOUCH_SLEEP_HOLD_MS/);
  assert.match(s3,/hold -> DEEP SLEEP/);
  assert.doesNotMatch(s3,/TOUCH_START_HOLD_MS = 2000/,'2-second start gesture must be removed from final source');
  assert.doesNotMatch(s3,/hold for 5 seconds to stay awake/,'deep-sleep wake must no longer require a five-second hold');
});

test('remote standby is a distinct BLE-wakeable state',()=>{
  const s3=productionS3();
  assert.match(s3,/CMD_STANDBY = 0x03/);
  assert.match(s3,/CMD_WAKE = 0x04/);
  assert.match(s3,/STANDBY=4/);
  assert.match(s3,/remoteStandby = false/);
  assert.match(s3,/void enterRemoteStandby\(\)/);
  assert.match(s3,/bool exitRemoteStandby\(\)/);
  assert.match(s3,/POWER_EVENT_MAGIC = 0xE2/);
  assert.match(s3,/publishPowerEvent\(POWER_STATE_DEEP_SLEEP\)/);
  assert.match(s3,/remote standby; BLE remains available/);
  assert.match(s3,/single tap -> wake from remote standby/);
});

test('power controls preserve build 1113 microphone reliability contract',()=>{
  const s3=productionS3();
  assert.match(s3,/MIC_START_ATTEMPTS=3/);
  assert.match(s3,/microphoneRecoveryUsed=false/);
  const stopCase=s3.slice(s3.indexOf('case CMD_STOP:'),s3.indexOf('break;',s3.indexOf('case CMD_STOP:'))+6);
  assert.doesNotMatch(stopCase,/stopMicrophone\(\)/,'normal recording STOP still must not cycle I2S');
  assert.match(s3,/enterRemoteStandby[\s\S]*?stopMicrophone\(\)/,'explicit standby may shut I2S down for power saving');
  assert.match(s3,/exitRemoteStandby[\s\S]*?startMicrophone\(\)/,'remote wake must restart I2S through the hardened retry path');
});

test('C3 gets the same gesture and standby protocol with its audited wake API',()=>{
  const s3=productionS3();
  const c3=materialize(s3,'esp32c3-supermini-4m');
  assert.match(c3,/CMD_STANDBY = 0x03/);
  assert.match(c3,/STANDBY=4/);
  assert.match(c3,/double tap -> START/);
  assert.match(c3,/single touch wake/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});
