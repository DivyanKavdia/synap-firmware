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
const {patch:failClosed}=require('../tools/patch-power-failclosed.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
function productionS3(){
  let s=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden,power,failClosed])s=fn(s);
  return s;
}

test('S3 deep sleep is fail-closed before BLE teardown',()=>{
  const s3=productionS3();
  assert.match(s3,/#include <Preferences\.h>/);
  assert.match(s3,/SYNAP_SLEEP_LOCK_KEY\[\] = "sleep-lock"/);
  assert.match(s3,/writeDurableSleepLock\(true\)/);
  assert.match(s3,/synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER/);
  assert.match(s3,/sleep lock committed request=/);
  assert.match(s3,/armTouchWakeSource\(\)/);
  assert.match(s3,/publishPowerEvent\(POWER_STATE_DEEP_SLEEP\)/);
  assert.match(s3,/esp_deep_sleep_start\(\)/);
  assert.doesNotMatch(s3,/BLEDevice::deinit\(true\)/);
  const lockAt=s3.indexOf('writeDurableSleepLock(true)');
  const eventAt=s3.indexOf('publishPowerEvent(POWER_STATE_DEEP_SLEEP)',lockAt);
  const sleepAt=s3.indexOf('esp_deep_sleep_start()',eventAt);
  assert(lockAt>0 && eventAt>lockAt && sleepAt>eventAt,'durable lock must precede app notification and deep sleep');
});

test('S3 sleep lock survives resets and clears only after validated triple tap',()=>{
  const s3=productionS3();
  assert.match(s3,/sleep lock survived a non-touch reset; returning to deep sleep before BLE/);
  assert.match(s3,/const bool sleepResume=durableLock \|\| \(synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER\) \|\| bootSleepWasLocked/);
  assert.match(s3,/if \(!touchWake\) \{[\s\S]*armTouchWakeAndSleep\(\);[\s\S]*return false;/);
  assert.match(s3,/could not clear durable sleep lock; refusing BLE boot/);
  const confirm=s3.slice(s3.indexOf('bool confirmTouchWakeTripleTap()'),s3.indexOf('void publishPowerEvent'));
  const clearAt=confirm.indexOf('writeDurableSleepLock(false)');
  const thirdTapAt=confirm.indexOf('if (taps!=3)');
  assert(clearAt>thirdTapAt,'durable lock must not clear until the third wake tap validates');
  assert.match(s3,/if \(!confirmTouchWakeTripleTap\(\)\) return;/);
  assert(s3.indexOf('if (!confirmTouchWakeTripleTap()) return;') < s3.lastIndexOf('initializeBLE();'),'BLE must initialize only after wake validation');
});

test('S3 touch wake uses GPIO13 without an internal RTC pulldown',()=>{
  const s3=productionS3();
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(s3,/rtc_gpio_init\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\)\)/);
  assert.match(s3,/RTC_GPIO_MODE_INPUT_ONLY/);
  assert.match(s3,/rtc_gpio_pulldown_dis\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\)\)/);
  assert.doesNotMatch(s3,/rtc_gpio_pulldown_en\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\)\)/);
  assert.match(s3,/esp_sleep_enable_ext0_wakeup\(static_cast<gpio_num_t>\(TOUCH_INPUT_PIN\),1\)/);
});

test('diagnostics retain reset and power-transition evidence',()=>{
  const s3=productionS3();
  assert.match(s3,/RTC_DATA_ATTR uint32_t synapSleepRequestCounter = 0/);
  assert.match(s3,/RTC_DATA_ATTR uint8_t synapLastSleepStage = 0/);
  assert.match(s3,/RTC_DATA_ATTR uint8_t synapLastWakeCause = 0/);
  assert.match(s3,/bootWakeCause=esp_sleep_get_wakeup_cause\(\)/);
  assert.match(s3,/if \(bootSleepWasLocked\) flags\|=0x10/);
  assert.match(s3,/if \(bootWakeCause==ESP_SLEEP_WAKEUP_EXT0\) flags\|=0x20/);
});

test('secondary target remains materializable after fail-closed patch',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/writeDurableSleepLock\(true\)/);
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup\(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH\)/);
  assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});

test('release workflow applies fail-closed patch after power controls and before materialization',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const powerAt=workflow.indexOf('patch-power-controls-v2.cjs');
  const closedAt=workflow.indexOf('patch-power-failclosed.cjs');
  const materializeAt=workflow.indexOf('materialize-target.cjs --check');
  assert(powerAt>0 && closedAt>powerAt && materializeAt>closedAt);
});