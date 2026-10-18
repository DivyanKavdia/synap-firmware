'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');

test('slider boot does no automatic FAT unlink, only read validation and explicit deletion',()=>{
 for(const src of [detect,ino]){
  const boot=src.split('bool odysseyInitializeSdCardBeforeBle() {')[1]
   .split('bool odysseyRecoverSdCard(const char* reason) {')[0];
  assert.match(boot,/const bool ready=odysseySdMountLocked\("boot",ODYSSEY_SD_BOOT_ATTEMPTS\)/);
  assert.match(boot,/odysseySdBootCleanupState=4/);
  assert.match(boot,/odysseySdBootCleanupRemoved=0/);
  assert.match(boot,/odysseySdMarkHistoricalFaultResolved\(\)/);
  assert.match(boot,/odysseySdRememberCleanupUnsafe\(false\)/);
  assert.doesNotMatch(boot,/unlink\(|odysseySdColdCleanupLocked\(|fopen\(|fwrite\(|fsync\(|esp_vfs_fat_sdcard_format\(/);
  assert.doesNotMatch(src,/static bool odysseySdColdCleanupLocked\(/);
  assert.doesNotMatch(src,/odysseySdCleanupName\(|odysseySdColdCleanupPartName\(/);
  assert.match(src,/odysseySdUnresolvedHistoricalStorageFault\(\)/);
 }
});

test('SD filesystem operations are independent of battery-voltage estimate',()=>{
 const remove=transfer.split('static uint8_t removeFile(const char* path) {')[1]
    .split('static uint16_t clearRecordings()')[0];
 const clear=transfer.split('static uint16_t clearRecordings() {')[1]
    .split('static void notifySdWindow(')[0];
 const worker=transfer.split('static void worker(void*) {')[1].split('class CommandCallbacks :')[0];
 for(const body of [remove,clear,worker]){
  assert.doesNotMatch(body,/batteryAvailable|batteryMillivolts|odysseySdPowerSafe|ODYSSEY_SD_WRITE_START_MIN_MV/);
 }
 assert.match(remove,/odysseySdWriteTimeoutQuarantined\(\)/);
 assert.match(remove,/odysseySdReadTimeoutQuarantined\(\)/);
 assert.match(worker,/ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED/);
 assert.match(worker,/case 17:/);
});

test('disabled cold cleanup is visible in read-only recorder diagnostics',()=>{
 const op=transfer.split('case 34: {')[1].split('case 29: {')[0];
 for(const field of ['cleanupRemoved','cleanupStatus','coldRecovery','resetReason']){
  assert(op.includes('\"'+field+'\"'),field);
 }
 assert.match(op,/odysseySdColdCleanupRemoved\(\)/);
 assert.match(op,/odysseySdColdCleanupStatus\(\)/);
 assert.doesNotMatch(op,/opendir\(|unlink\(|fwrite\(|odysseyRecoverSdCard\(/);
});
