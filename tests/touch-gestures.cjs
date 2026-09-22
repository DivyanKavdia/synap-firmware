'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for (const {target,c3,pin} of [
  {target:'esp32s3-fh4r2-qspi-4m',c3:false,pin:13},
  {target:'esp32c3-supermini-4m',c3:true,pin:3},
  {target:'xiao-esp32s3-sense-8m',c3:false,pin:0},
]) {
  test(`${target} validates immediate double taps and four-second sleep/wake holds`,()=>{
    const code=materialize(source,target);
    const wake=code.slice(code.indexOf('bool confirmTouchWakeGesture() {'),code.indexOf('void publishPowerEvent('));
    const poll=code.slice(code.indexOf('void pollTouchControl() {'),code.indexOf('void updateStatusCharacteristic(bool notify) {'));
    const fixture=fs.readFileSync(path.join(__dirname,'touch-gestures.cpp'),'utf8');
    const flags=[`-DCONFIG_IDF_TARGET_ESP32C3=${c3?1:0}`,`-DCONFIG_IDF_TARGET_ESP32S3=${c3?0:1}`,`-DSYNAP_TOUCH_TEST_PIN=${pin}`];
    assert.match(nativeTest(fixture.replace('// INSERT WAKE',wake).replace('// INSERT POLL',poll),flags),/PASS shared touch/);
    assert.doesNotMatch(code,/pendingDoubleAt|touch-triple|WAKE_TRIPLE/);
  });
}
