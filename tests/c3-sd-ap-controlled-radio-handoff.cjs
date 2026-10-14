'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const compiled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');

test('BLE request stages credentials and reboots after bounded SD quiesce, without allocating SoftAP beside BLE',()=>{
  for(const source of [ap,compiled]){
    const task=source.split('static void apTask(void*) {')[1].split('bool resumeAtBoot() {')[0];
    assert.match(task,/vTaskDelay\(pdMS_TO_TICKS\(4000\)\)/);
    assert.match(task,/odysseyPrepareSdForPowerTransition\(1500u\)/);
    assert.match(task,/rtcHandoff\.magic=AP_REBOOT_MAGIC/);
    assert.match(task,/ESP\.restart\(\)/);
    assert.doesNotMatch(task,/WiFi\.mode\(|WiFi\.softAP\(|activateAp\(/);
    const enable=source.split('static bool activateAp() {')[1].split('static void apTask(void*) {')[0];
    assert.match(enable,/if \(!bootExclusive\)/);
    assert.match(enable,/WiFi\.mode\(WIFI_AP\)/);
    assert.match(enable,/WiFi\.softAP\(next\.ssid,next\.password,1,false,1\)/);
    assert.doesNotMatch(enable,/pauseBleForWifi\(\)/);
    assert.doesNotMatch(enable,/if\s*\([^)]*OdysseyWifi::busy\(\)/);
  }
});

test('Wi-Fi-only boot consumes RTC marker and never initializes BLE before AP',()=>{
  for(const source of [ap,compiled]){
    const resume=source.split('bool resumeAtBoot() {')[1].split('void runBootSession() {')[0];
    const run=source.split('void runBootSession() {')[1].split('bool begin() {')[0];
    assert.match(resume,/bootResetReason==ESP_RST_SW/);
    assert.match(resume,/rtcHandoff\.inverse==~AP_REBOOT_MAGIC/);
    assert.match(resume,/clearRtcHandoff\(\)/);
    assert.match(resume,/bootExclusive=true/);
    assert.match(run,/odysseyInitializeSdCardBeforeBle\(\)/);
    assert.match(run,/activateAp\(\)/);
    assert.match(run,/serveSession\(\)/);
    assert.match(run,/odysseyPrepareSdForPowerTransition\(1500u\)/);
    assert.match(run,/ESP\.restart\(\)/);
    assert.match(source,/RTC_DATA_ATTR static RtcHandoff rtcHandoff/);
    assert.doesNotMatch(source,/struct RtcHandoff\s*\{[^}]*=/);
    assert.match(source,/OdysseySdGuard sd\(pdMS_TO_TICKS\(1500\)\)/);
  }
  const setup=boot.split('void setup() {')[1];
  const exclusive=setup.indexOf('OdysseyWifiAp::resumeAtBoot()');
  assert(exclusive>0 && exclusive<setup.indexOf('initializeBLE();'));
  assert(exclusive<setup.indexOf('audioFrameQueue=xQueueCreate'));
  assert(exclusive<setup.indexOf('OdysseyTransfer::initialize();'));
});

test('Wi-Fi mode stores failure stage for BLE diagnosis across software restart and never modifies recordings',()=>{
  for(const source of [ap,compiled]){
    assert.match(source,/rtcFailure=stage;rtcFailureHeap=freeHeap;rtcFailureLargest=largest/);
    assert.match(source,/lastStartFailure\.load\(\)\?lastStartFailure\.load\(\):rtcFailure/);
    assert.match(source,/rtcFailure=0;rtcFailureHeap=0;rtcFailureLargest=0/);
    assert.match(source,/if\(!bootExclusive\) resumeBleAfterWifi\(\)/);
    if(source===ap)assert.doesNotMatch(source,/\bfwrite\s*\(|\bunlink\s*\(|\bremove\s*\(|sdcard_format/);
  }
});
