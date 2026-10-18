'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');

test('slider cold boot removes only empty generated WAV / PART; usable partial audio is retained',()=>{
 for(const src of [detect,ino]){
  const scan=src.split('static bool odysseySdColdCleanupLocked() {')[1]
    .split('bool odysseyInitializeSdCardBeforeBle() {')[0];
  assert.match(scan,/bootResetReason!=ESP_RST_POWERON/);
  assert.match(scan,/!odysseySdReady\(\)/);
  assert.match(scan,/odysseySdTimedOutWriteQuarantined\.load\(\)/);
  assert.match(scan,/odysseySdTimedOutReadQuarantined\.load\(\)/);
  assert.match(scan,/odysseySdCleanupName\(e->d_name,part\)/);
  assert.match(scan,/odysseySdColdCleanupPartName\(e->d_name\)/);
  assert.match(scan,/if \(st\.st_size<0 \|\| st\.st_size>44\) continue/);
  assert.doesNotMatch(scan,/if \(!part && \(st\.st_size<0 \|\| st\.st_size>44\)\)/);
  assert.match(scan,/closedir\(dir\)/);
  assert(scan.indexOf('closedir(dir)')<scan.indexOf('unlink(candidates[i])'));
  assert.match(scan,/if \(found && !odysseySdRememberCleanupUnsafe\(true\)\)/);
  assert(scan.indexOf('odysseySdRememberCleanupUnsafe(true)')<scan.indexOf('unlink(candidates[i])'));
  assert.match(scan,/odysseySdQuarantineUncertainWrite\(\)/);
  assert.doesNotMatch(scan,/format\(|esp_vfs_fat_sdcard_format\(|odysseyRecoverSdCard\(/);
  const boot=src.split('bool odysseyInitializeSdCardBeforeBle() {')[1]
    .split('bool odysseyRecoverSdCard(const char* reason) {')[0];
  assert(boot.indexOf('odysseySdMountLocked("boot"')<boot.indexOf('odysseySdColdCleanupLocked()'));
  assert.match(boot,/odysseySdMarkHistoricalFaultResolved\(\)/);
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

test('cold empty cleanup is included in read-only recorder diagnostics',()=>{
 const op=transfer.split('case 34: {')[1].split('case 29: {')[0];
 for(const field of ['cleanupRemoved','cleanupStatus','coldRecovery','resetReason']){
  assert(op.includes('\\"'+field+'\\"'),field);
 }
 assert.match(op,/odysseySdColdCleanupRemoved\(\)/);
 assert.match(op,/odysseySdColdCleanupStatus\(\)/);
 assert.doesNotMatch(op,/opendir\(|unlink\(|fwrite\(|odysseyRecoverSdCard\(/);
});
