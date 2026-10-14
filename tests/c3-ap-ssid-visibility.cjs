'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const ble=fs.readFileSync('firmware/shared/ble-control.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('C3 AP uses core default 192.168.4.1 and never adds an unnecessary DHCP config failure',()=>{
 for(const source of [ap,ino]){
  const start=source.split('static bool activateAp() {')[1].split('static void apTask(void*) {')[0];
  assert.match(start,/WiFi\.mode\(WIFI_AP\)/);
  assert.match(start,/WiFi\.softAP\(next\.ssid,next\.password,1,false,1\)/);
  assert.doesNotMatch(start,/softAPConfig\(/);
  assert.match(start,/IPAddress expected\(192,168,4,1\)/);
  assert.match(start,/WiFi\.softAPIP\(\)!=expected/);
  assert.match(start,/vTaskDelay\(pdMS_TO_TICKS\(50\)\)/);
  assert.match(start,/noteStartFailure\(4\)/);
  assert.match(start,/noteStartFailure\(5\)/);
  assert.match(start,/noteStartFailure\(6\)/);
 }
});
test('a failed AP startup is inspectable via read-only BLE op21 after the link returns',()=>{
 for(const source of [ap,ino]){
  assert.match(source,/static void noteStartFailure\(uint8_t stage\)/);
  assert.match(source,/heap_caps_get_largest_free_block\(MALLOC_CAP_8BIT\)/);
  assert.match(source,/lastStartFailure=0;failedHeap=0;failedLargestBlock=0/);
  assert.match(source,/lastStartFailure\.load\(\)/);
  assert.match(source,/failedLargestBlock\.load\(\)/);
  assert.match(source,/noteStartFailure\(8\)/);
  assert.match(source,/noteStartFailure\(9\)/);
 }
});
test('Bluedroid auto advertising cannot race a staged C3 SoftAP startup',()=>{
 for(const source of [ble,ino]){
  const control=source.split('void controlTask(void* parameter) {')[1].split('#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU\n// SYNAP_C3_INDEPENDENT_TOUCH_STOP')[0];
  assert.match(control,/restartAdvertising && !deviceConnected\.load\(\)/);
  assert.match(control,/bleAutoAdvertiseAllowed\(\)/);
  assert.match(source,/static bool bleAutoAdvertiseAllowed\(\)/);
  assert.match(source,/return !OdysseyWifi::busy\(\)/);
  assert.match(control,/bleServer->startAdvertising\(\)/);
 }
 assert.match(ap,/void shutDown\(\)/);
 assert.match(ap,/resumeBleAfterWifi\(\)/);
 assert.doesNotMatch(ap,/\bfwrite\s*\(|\bunlink\s*\(|\bremove\s*\(|sdcard_format/);
});
