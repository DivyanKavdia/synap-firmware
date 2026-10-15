'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const caps=fs.readFileSync('firmware/shared/module-capabilities.cpp','utf8');
test('C3 BLE-only protocol removes hotspot and STA command handlers',()=>{
 assert.doesNotMatch(tx,/case 20:|case 23:|case 24:|case 26:|OdysseyWifiAp::/);
 assert.doesNotMatch(caps,/p\[16\]\|=2|OdysseyWifi::available\(\)/);
});
test('C3 fast SD receiver grants bounded credits without changing legacy windows',()=>{
 assert.match(tx,/case 12:\s*case 30:/);
 assert.match(tx,/request\.credits/);
 assert.match(tx,/uint8_t\(12\)/);
 assert.match(tx,/vTaskDelay\(pacing\)/);
 assert.match(tx,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
});
