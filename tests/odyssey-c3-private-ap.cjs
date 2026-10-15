'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const sta=fs.readFileSync('firmware/shared/odyssey-c3-wifi.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
test('C3 retirement removes credentials and reboot-exclusive Wi-Fi mode',()=>{
 assert.doesNotMatch(ap,/RTC_NOINIT_ATTR|RTC_DATA_ATTR|WiFi|WebServer|192\.168|ssid\[/);
 assert.doesNotMatch(sta,/WiFiClientSecure|Preferences|WiFi\.mode|setCACert/);
 assert.doesNotMatch(boot,/OdysseyWifiAp::resumeAtBoot|OdysseyWifiAp::runBootSession/);
 assert.doesNotMatch(tx,/OdysseyWifiAp::|case 20:|request\.operation==21|request\.operation==22/);
});
test('read-only C3 SD BLE sync retains both root and legacy WAV paths',()=>{
 assert.match(tx,/odysseySdRootWavName\(name\)/);
 assert.match(tx,/odysseySdMountPoint\(\),ODYSSEY_SD_RECORDING_DIR/);
 assert.match(tx,/ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
});
