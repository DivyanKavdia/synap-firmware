'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const compiled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('ESP32-C3 hotspot quiesces idle BLE and stops advertisements before starting Wi-Fi',()=>{
  for(const source of [ap,compiled]){
    const pause=source.split('static bool pauseBleForWifi() {')[1].split('static void resumeBleAfterWifi()')[0];
    const enable=source.split('static bool activateAp() {')[1].split('static void apTask(void*) {')[0];
    assert(pause);
    assert.match(pause,/advertiseOnDisconnect\(false\)/);
    assert.match(pause,/BLEDevice::stopAdvertising\(\)/);
    assert.match(pause,/bleServer->disconnect\(connId\)/);
    assert.match(pause,/getConnectedCount\(\)/);
    assert.match(pause,/uint32_t\(millis\(\)-began\)<2000u/);
    assert.match(pause,/return false;/);
    assert(enable.indexOf('pauseBleForWifi()')<enable.indexOf('WiFi.mode(WIFI_AP)'));
    // The composite OdysseyWifi::busy() includes OdysseyWifiAp::busy().
    // Calling it after begin() arms apActive would always block SoftAP.
    assert.doesNotMatch(enable,/if\s*\([^)]*OdysseyWifi::busy\(\)/);
    assert.match(enable,/!odysseySdReady\(\)/);
    assert.match(source,/vTaskDelay\(pdMS_TO_TICKS\(4000\)\)/);
  }
});
test('Wi-Fi teardown restores BLE advertising, with a single idempotent cleanup owner',()=>{
  for(const source of [ap,compiled]){
    const resume=source.split('static void resumeBleAfterWifi() {')[1].split('static void shutDown() {')[0];
    const shutdown=source.split('static void shutDown() {')[1].split('static void serveSession() {')[0];
    const serve=source.split('static void serveSession() {')[1].split('static bool activateAp() {')[0];
    const task=source.split('static void apTask(void*) {')[1].split('bool begin() {')[0];
    assert.match(resume,/advertiseOnDisconnect\(true\)/);
    assert.match(resume,/bleServer->startAdvertising\(\)/);
    assert(shutdown.indexOf('WiFi.mode(WIFI_OFF)')<shutdown.indexOf('resumeBleAfterWifi()'));
    assert.doesNotMatch(serve,/shutDown\(\)/);
    assert.equal((task.match(/shutDown\(\)/g)||[]).length,1);
    assert.match(source,/OdysseySdGuard sd\(pdMS_TO_TICKS\(1500\)\)/);
    if (source===ap) assert.doesNotMatch(source,/\bfwrite\s*\(|\bunlink\s*\(|\bremove\s*\(|sdcard_format/);
  }
});
