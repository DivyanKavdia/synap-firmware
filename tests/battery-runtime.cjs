'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for(const [c3,disabled] of [[false,false],[true,false],[true,true]]){
  test(`battery sampling uses the actual target conversion and policy (C3=${c3}, disabled=${disabled})`,()=>{
    const code=materialize(source,c3?'esp32c3-supermini-4m':'esp32s3-fh4r2-qspi-4m');
    const battery=code.slice(code.indexOf('uint8_t batteryPercentFromMillivolts('),code.indexOf('bool armTouchWakeSource() {'));
    const attenuation=code.match(/analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_\w+\);/)[0];
    const fixture=fs.readFileSync(path.join(__dirname,'battery-runtime.cpp'),'utf8');
    const flags=[`-DCONFIG_IDF_TARGET_ESP32C3=${c3?1:0}`,`-DCONFIG_IDF_TARGET_ESP32S3=${c3?0:1}`];
    if(disabled)flags.push('-DSYNAP_BATTERY_MONITOR_ENABLE=0');
    assert.match(nativeTest(fixture.replace('// INSERT CONFIGURATION',`void configureBatteryAdc(){${attenuation}}`).replace('// INSERT BATTERY',battery),flags),/PASS battery/);
    if(c3){
      assert.match(code,/#define SYNAP_BATTERY_ADC_PIN 1/);
      assert.doesNotMatch(code,/BATTERY_CAL_ADC_MV|raw 1544/);
      assert.match(code,/C3_SLEEP_HOLD_MS = 4000/);
    }
  });
}
