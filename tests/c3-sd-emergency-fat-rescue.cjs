'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
test('normal boot remains read-only; unmountable FAT is classified separately',()=>{
 assert.match(sd,/mount\.format_if_mount_failed=false/);
 assert.match(sd,/if \(explicitlyProvisionFat\) mount\.format_if_mount_failed=true/);
 assert.match(sd,/odysseySdLastMountError\.load\(\)==ESP_FAIL/);
 assert.match(sd,/odysseySdBootState=2;odysseySdProbeStage=3/);
});
test('explicit emergency FAT rescue refuses warm boots, read/write quarantine and repeats',()=>{
 const fn=sd.split('esp_err_t odysseySdEmergencyProvisionFat() {')[1].split('// Explicit media-v1 operation 19 only')[0];
 for(const key of ['esp_reset_reason()!=ESP_RST_POWERON','odysseySdProbeStage.load()!=3','odysseySdLastMountError.load()!=ESP_FAIL','odysseySdTimedOutReadQuarantined.load()','odysseySdTimedOutWriteQuarantined.load()','odysseySdControllerBusyLow()','odysseyRecording.load()','odysseySdEmergencyProvisionAttempted=true','odysseySdBeginLocked(true)','odysseySdValidateVfsLocked("emergency-format",1)','odysseySdMarkHistoricalFaultResolved()'])assert(fn.includes(key),key);
 assert.doesNotMatch(fn,/odysseyPersistRecordFailure\(0,0\)/);
});
test('emergency media op33 requires token, leaves 17/18 blocked and runs once',()=>{
 assert.match(tx,/ODYSSEY_C3_SD_RESCUE_TOKEN=0x53445246u/);
 const guard=tx.split('if ((!ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED &&')[1].split('sdBleTransferInFlight=true;')[0];
 for(const n of [17,18,19,33])assert(guard.includes('request.operation=='+n+'u'),String(n));
 assert.match(guard,/request.offset!=ODYSSEY_C3_SD_RESCUE_TOKEN/);
 const operation=tx.split('case 33: {')[1].split('case 32: {')[0];
 assert.match(operation,/odysseySdEmergencyProvisionFat\(\)/);
 assert.doesNotMatch(operation,/odysseyRecoverSdCard\(|odysseyFormatSdCard\(/);
});

test('an SD card transport error during mount must block emergency FAT formatting',()=>{
 const handler=sd.split('esp_err_t odysseySdEmergencyProvisionFat() {')[1]
   .split('// Explicit media-v1 operation 19 only')[0];
 assert.match(handler,/odysseyWireError\.load\(\)!=0/);
 assert.match(sd,/odysseySdProbeStage=odysseyWireError\.load\(\)\?4:3/);
});

test('emergency FAT rescue may rebuild corrupted VFS only after explicit authorization',()=>{
 const rescue=sd.split('esp_err_t odysseySdEmergencyProvisionFat() {')[1]
   .split('// Explicit media-v1 operation 19 only')[0];
 assert.match(rescue,/odysseySdProbeStage\.load\(\)!=3 && odysseySdProbeStage\.load\(\)!=4/);
 assert.match(rescue,/initialized && !verified && odysseySdNativeCard/);
 assert.match(rescue,/odysseyWireError\.load\(\)==0/);
 assert.match(rescue,/esp_vfs_fat_sdcard_format\(/);
 assert.match(rescue,/odysseySdValidateVfsLocked\("emergency-format",1\)/);
});
