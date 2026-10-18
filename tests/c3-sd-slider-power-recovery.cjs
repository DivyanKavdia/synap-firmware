'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 slider power-on cold-recovery records only actual verified mount',()=>{
 for (const source of [sd,ino]){
   const cold=source.split('bool odysseyInitializeSdCardBeforeBle() {')[1].split('bool odysseyRecoverSdCard(')[0];
   assert.match(cold,/odysseySdUnresolvedHistoricalStorageFault\(\)/);
   assert.match(cold,/if \(bootResetReason!=ESP_RST_POWERON\)/);
   assert.match(cold,/odysseySdColdRecoveryState=3/);
   assert.match(cold,/odysseySdColdRecoveryState=2/);
   assert.match(cold,/odysseySdMountLocked\("boot",ODYSSEY_SD_BOOT_ATTEMPTS\)/);
   assert.match(cold,/if \(ready && bootResetReason==ESP_RST_POWERON\)/);
   assert.match(cold,/odysseySdMarkHistoricalFaultResolved\(\)/);
   assert.match(cold,/if \(previousStorageFault\) odysseySdColdRecoveryState=1/);
   assert(!/unlink\(|fopen\(|fwrite\(|esp_vfs_fat_sdcard_format\(/.test(cold));
 }
});
test('A warm reset cannot evade persisted CMD25 quarantine via touch or BLE retry',()=>{
 for(const source of [sd,ino]){
   const recover=source.split('bool odysseyRecoverSdCard(const char* reason) {')[1]
      .split('// An explicit emergency FAT rescue')[0];
   assert.match(recover,/!odysseySdReady\(\) && odysseySdUnresolvedHistoricalStorageFault\(\)/);
   assert.match(recover,/bootResetReason!=ESP_RST_POWERON/);
   assert.match(recover,/odysseySdColdRecoveryState\.load\(\)==2u/);
   assert(recover.indexOf('manual recovery refused')<recover.indexOf('OdysseySdGuard guard'),
      'history and reset-kind gate must precede ALL SDSPI access');
   assert(recover.indexOf('manual recovery refused')<recover.indexOf('odysseySdReleaseLocked()'),
      'manual retry must not tear down an unknown controller');
   assert.match(source,/odysseySdHistoricalFaultResolved\(\)/);
 }
});
test('A current native timeout forbids GPIO CMD0 fallback during cold FAT initialization',()=>{
 const mount=sd.split('static bool odysseySdMountOnceLocked(const char* reason,uint8_t attempt) {')[1]
  .split('static bool odysseySdMountLocked(')[0];
 assert.match(mount,/if \(odysseySdTimedOutWriteQuarantined\.load\(\) \|\|/);
 assert.match(mount,/odysseySdTimedOutReadQuarantined\.load\(\)\)/);
 assert(mount.indexOf('cold mount native timeout')<mount.indexOf('odysseySdBitBangRecoverLocked(reason)'));
});
test('BLE op34 exposes cold recovery state and reset reason without touching FAT',()=>{
 for(const source of [transfer,ino]){
  const health=source.split('case 34: {')[1].split('case 29: {')[0];
  for(const key of ['coldRecovery','resetReason','writeQuarantine','readQuarantine'])
    assert(health.includes('\\\"'+key+'\\\"'),'missing '+key);
  assert.match(health,/odysseySdColdRecoveryStatus\(\)/);
  assert.match(health,/unsigned\(bootResetReason\)/);
  assert.doesNotMatch(health,/odysseyRecoverSdCard\(|sdspi_host_do_transaction|unlink\(|fopen\(|fwrite\(|esp_vfs_fat_sdspi_mount/);
 }
});
