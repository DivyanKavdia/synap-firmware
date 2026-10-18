'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const combined=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 no longer reboots into a Wi-Fi-only boot; BLE remains available',()=>{
 for(const source of [boot,combined]) {
  assert.match(source,/initializeBLE\(\)/);
  assert.doesNotMatch(source,/OdysseyWifiAp::resumeAtBoot|OdysseyWifiAp::runBootSession/);
 }
 assert.doesNotMatch(tx,/OdysseyWifiAp::/);
});
test('C3 still protects offline recordings during BLE SD reads',()=>{
 assert.match(tx,/odysseyRecording\.load\(\)/);
 assert.match(tx,/OdysseySdGuard guard/);
 assert.match(tx,/ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED=false/);
});
