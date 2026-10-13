'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const compiled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 staged SoftAP never runs Wi-Fi.mode in BLE command and returns credentials in op20',()=>{
 for(const source of [ap,compiled]){
  const begin=source.split('namespace OdysseyWifiAp {')[1].split('bool begin() {')[1].split('bool stop() {')[0];
  assert(begin);
  assert.match(begin,/next.active=true/);
  assert.match(begin,/store\(next\)/);
  assert.match(begin,/xTaskCreate\(apTask/);
  assert.doesNotMatch(begin,/WiFi\.mode\(|WiFi\.softAP\(/);
  assert.match(source,/vTaskDelay\(pdMS_TO_TICKS\(4000\)\)/);
  assert.match(source,/static bool activateAp\(\)/);
  assert.match(source,/\\\"starting\\\":%s/);
 }
 const op20=tx.split('case 20: {')[1].split('case 27:')[0];
 assert(op20);
 assert.match(op20,/OdysseyWifiAp::begin\(\)/);
 assert.match(op20,/OdysseyWifiAp::encode/);
 assert.match(op20,/reply\(request,OK,uint32_t\(n\),0/);
 assert.match(op20,/continue;/);
 assert.doesNotMatch(op20,/WiFi\.mode|delay\(/);
});
test('Private SoftAP remains read-only, mutex-serialized and shut down after task',()=>{
 const task=ap.split('static void apTask(void*) {')[1].split('bool begin() {')[0];
 assert(task.indexOf('activateAp()')<task.indexOf('shutDown()'));
 assert.match(ap,/OdysseySdGuard sd\(pdMS_TO_TICKS\(1500\)\)/);
 assert.match(ap,/void shutDown/);
 assert.doesNotMatch(ap,/\bunlink\s*\(|\bremove\s*\(|\bmkdir\s*\(|\bfwrite\s*\(/);
});
