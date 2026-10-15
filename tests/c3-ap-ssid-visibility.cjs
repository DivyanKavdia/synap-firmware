'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const source=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
test('C3 boot stays BLE-only with no AP handshake or RTC session',()=>{
 assert.match(boot,/initializeBLE\(\)/);
 assert.match(boot,/odysseyInitializeSdCardBeforeBle\(\)/);
 assert.doesNotMatch(boot,/resumeAtBoot\(|runBootSession\(/);
 assert.doesNotMatch(source,/WebServer\(80\)|WiFi\.softAP\(|RTC_NOINIT_ATTR static RtcHandoff/);
 assert.doesNotMatch(ap,/#include <WiFi\.h>|WiFi\.mode/);
});
