'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for(const [c3,disabled] of [[false,false],[true,false],[true,true]]){
  test(`battery sampling uses the actual target conversion and policy (C3=${c3}, disabled=${disabled})`,()=>{
    const code=materialize(source,c3?'esp32c3-supermini-4m':'esp32s3-fh4r2-qspi-4m');
    const battery=code.slice(code.indexOf('// SYNAP_BATTERY_RUNTIME_BEGIN'),code.indexOf('bool armTouchWakeSource() {'));
    // The shared boot uses a C3/S3 conditional: inspect both target branches,
    // then substitute only the active branch in the native harness.
    assert.match(code, /#if CONFIG_IDF_TARGET_ESP32C3\\s+analogSetPinAttenuation\\(BATTERY_ADC_PIN, ADC_11db\\);\\s+#else\\s+analogSetPinAttenuation\\(BATTERY_ADC_PIN, ADC_6db\\);\\s+#endif/);
    const attenuation = `analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_${c3 ? '11db' : '6db'});`;
    const profile=require('../tools/device-profile.cjs').profileBlock(code).split('\n').filter(line=>!line.startsWith('constexpr ')).join('\n');
    const fixture=fs.readFileSync(path.join(__dirname,'battery-runtime.cpp'),'utf8');
    const flags=[`-DCONFIG_IDF_TARGET_ESP32C3=${c3?1:0}`,`-DCONFIG_IDF_TARGET_ESP32S3=${c3?0:1}`];
    if(disabled)flags.push('-DSYNAP_BATTERY_MONITOR_ENABLE=0');
    assert.match(nativeTest(fixture.replace('// INSERT CONFIGURATION',profile+'\n'+`void configureBatteryAdc(){${attenuation}}`).replace('// INSERT BATTERY',battery),flags),/PASS battery/);
    if(c3){
      assert.match(code,/#define SYNAP_BATTERY_ADC_PIN 1/);
      assert.match(code,/#define SYNAP_BATTERY_SCALE_NUMERATOR 2/);
      assert.match(code,/#define SYNAP_BATTERY_SCALE_DENOMINATOR 1/);
      assert.match(code,/#define SYNAP_BATTERY_FULL_MV 4150/);
      assert.match(code,/#define SYNAP_SD_BATTERY_SCALE_NUMERATOR 1470/);
      assert.match(code,/#define SYNAP_SD_BATTERY_SCALE_DENOMINATOR 470/);
      assert.match(code,/#define SYNAP_SD_BATTERY_FULL_MV 4200/);
      assert.doesNotMatch(code,/BATTERY_CAL_ADC_MV|raw 1544/);
      assert.match(code,/TOUCH_SLEEP_HOLD_MS = 4000/);
    }
  });
}
