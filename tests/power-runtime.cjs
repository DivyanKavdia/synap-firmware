'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
const led=source.slice(source.indexOf('void updateStatusLed(bool force) {'),source.indexOf('void setDeviceState(DeviceState state, ErrorCode error) {'));
const loop=source.slice(source.indexOf('void loop() {'));
const fixture=fs.readFileSync(path.join(__dirname,'power-runtime.cpp'),'utf8');
for(const [rollback,microphone] of [[0,1],[1,1],[1,0]]){
  test(`standby stays dark and housekeeping preserves boot validation (rollback=${rollback}, mic=${microphone})`,()=>{
    const body=fixture.replace('// INSERT LED',led).replace('// INSERT LOOP',loop);
    assert.match(nativeTest(body,[`-DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=${rollback}`,`-DUSE_REAL_I2S_MIC=${microphone}`]),/PASS power/);
  });
}
