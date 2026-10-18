'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const read=p=>fs.readFileSync(p,'utf8');
const sta=read('firmware/shared/odyssey-c3-wifi.cpp');
const ap=read('firmware/shared/odyssey-c3-sd-ap.cpp');
const media=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
const source=read('synap_esp32s3/synap_esp32s3.ino');
test('C3 firmware compiles no Wi-Fi STA/TLS or SoftAP stack on either OTA track',()=>{
 for(const text of [sta,ap,source])
  assert.doesNotMatch(text,/#include <WiFi\.h>|WiFiClientSecure|WebServer\(80\)|WiFi\.softAP\(|WiFi\.begin\(|WiFi\.mode\(/);
 assert.match(sta,/bool busy\(\) \{ return false; \}/);
 assert.doesNotMatch(media,/OdysseyWifiAp::|request\.operation==25|case 23:|case 24:|case 26:/);
});
test('BLE read-only fast op30 and legacy op12 do not modify FAT media',()=>{
 assert.match(media,/case 12:/);
 assert.match(media,/case 30:/);
 assert.match(media,/ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED=false/);
 assert.match(media,/readSelected\(windowPath,offset,total,chunk,length\)/);
 assert.match(media,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
});
