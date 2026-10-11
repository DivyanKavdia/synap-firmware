'use strict';
const { test }=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const boot=read('firmware/shared/boot.cpp');
const power=read('firmware/shared/power.cpp');
const sd=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
const runtime=read('firmware/shared/runtime.cpp');
const ino=read('synap_esp32s3/synap_esp32s3.ino');
const catalog=require('../devices/catalog.json');
const c3=catalog.devices.find(d=>d.id==='esp32c3-supermini-4m');

test('C3 cold power-on overrides stale NVS sleep-lock; actual deep-sleep wake still requires touch',()=>{
  assert.match(boot,/bootResetReason==ESP_RST_POWERON[\s\S]*?writeDurableSleepLock\(false\)/);
  assert.match(boot,/if \(bootResetReason==ESP_RST_POWERON\) bootSleepWasLocked=false/);
  assert.match(power,/if \(bootResetReason==ESP_RST_POWERON\)[\s\S]*?return true;/);
  assert.match(power,/touchWake=\(cause==ESP_SLEEP_WAKEUP_GPIO\)/);
  assert.match(power,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(power,/if \(uint32_t\(millis\(\)-pressedAt\)<TOUCH_WAKE_HOLD_MS\)/);
});

test('Failed early return-to-sleep falls back to discoverable BLE boot rather than inert setup',()=>{
  assert.match(power,/static bool resumeC3BootAfterFailedSleep\(const char\* reason\)/);
  assert.match(power,/sleepPending=false;[\s\S]*?return true;[\s\S]*?#else/);
  for(const reason of ['non-touch reset','short wake press','NVS wake lock'])
    assert(power.includes('resumeC3BootAfterFailedSleep("'+reason+'")'));
  assert.match(power,/if \(!armTouchWakeSource\(\)\)[\s\S]*?sleepPending=false;[\s\S]*?return;[\s\S]*?#else/);
  assert.match(power,/wake-gate sleep deferred: C3 SD is not idle"[\s\S]*?sleepPending=false;/);
  assert.match(power,/if \(retainedStage>=44u && retainedStage!=48u\)[\s\S]*?resumeC3BootAfterFailedSleep/);
});

test('C3 transfer owns an explicit sleep veto including request gaps and user power gesture',()=>{
  assert.match(runtime,/namespace OdysseyTransfer \{[\s\S]*?bool busy\(\)/);
  assert.match(transfer,/std::atomic<bool> sdBleTransferInFlight\{false\}/);
  assert.match(transfer,/sdBleTransferInFlight=true/);
  assert.match(transfer,/sdBleLastTransferAt=millis\(\)/);
  assert.match(transfer,/uint32_t\(millis\(\)-last\)<15000u/);
  assert.match(transfer,/case 12:\s*streamSdWindow\(request\);[\s\S]*?sdBleTransferInFlight=false/);
  assert.match(power,/if \(OdysseyTransfer::busy\(\)\)[\s\S]*?deep sleep deferred: C3 SD transfer active/);
  assert.match(power,/if \(OdysseyWifi::busy\(\) \|\| OdysseyTransfer::busy\(\)\) return/);
  assert.match(power,/deepSleepAfterStop=true;[\s\S]*?deep sleep deferred until C3 SD BLE sync completes/);
  const sleep=power.split('void enterDeepSleep(const char* reason) {')[1].split('void powerTick() {')[0];
  assert(sleep.indexOf('OdysseyTransfer::busy()')<sleep.indexOf('writeDurableSleepLock(true)'));
  assert(sleep.indexOf('odysseyPrepareSdForPowerTransition(1000u)')<sleep.indexOf('publishPowerEvent(POWER_STATE_DEEP_SLEEP)'));
});

test('A definitely held-low SD bus skips futile repeated boot probes but remains unsafe for sleep',()=>{
  const mount=sd.split('static bool odysseySdMountLocked(')[1].split('void odysseyDetectSdCard()')[0];
  assert.match(mount,/!strcmp\(reason,"boot"\)/);
  assert.match(mount,/odysseySdBitBangCsHigh\.load\(\)==0 && odysseySdRawZero\.load\(\)>=900u/);
  assert.match(mount,/odysseySdUnsafeToSleep=true;/);
  assert.match(sd,/odysseySdMountLocked\("boot",ODYSSEY_SD_BOOT_ATTEMPTS\)/);
});

test('Recorder, OTA identity, SD pin mapping and generated firmware sketch stay unchanged',()=>{
  assert.equal(c3.slotSize,1310720);
  assert.equal(c3.productMarker,'SYNAP-ESP32C3-OTA-ID-V3');
  assert.deepEqual(c3.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  // The assembler rewrites boot/profile declarations, so assert the exact
  // reusable function fragments and targeted boot/runtime behaviors instead.
  for(const p of ['power.cpp','odyssey-sd-1631-detect.cpp','odyssey-sd-1631-transfer.cpp']){
    const original=read('firmware/shared/'+p).trim();
    assert(ino.includes(original),'generated sketch must match '+p);
  }
  assert.match(ino,/bootResetReason==ESP_RST_POWERON/);
  assert.match(ino,/if \(bootResetReason==ESP_RST_POWERON\) bootSleepWasLocked=false/);
  assert.match(ino,/bool busy\(\); \/\/ in-flight SD BLE transfer/);
  assert.match(ino,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(ino,/ODYSSEY_SD_DATA_FREQ_HZ=1000000u/);
  assert.match(sd,/esp_vfs_fat_sdspi_mount/); assert.match(sd,/mount\.format_if_mount_failed=false/);
});
