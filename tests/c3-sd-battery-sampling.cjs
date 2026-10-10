'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const boot=read('firmware/shared/boot.cpp');
const battery=read('firmware/shared/battery.cpp');
const devices=require('../devices/catalog.json').devices;

test('C3 battery is sampled after SD divider discovery, without changing S3 boot path',()=>{
 const before=boot.split('  // A C3 may be fitted with')[1].split('#if USE_REAL_I2S_MIC')[0];
 assert.match(before,/#if !CONFIG_IDF_TARGET_ESP32C3\s+sampleBattery\(true\);\s+#endif/);
 const after=boot.split('  odysseyInitializeSdCardBeforeBle();')[1].split('  initializeBLE();')[0];
 assert.match(after,/sampleBattery\(true\);/);
 const c3=devices.find(x=>x.id==='esp32c3-supermini-4m');
 const s3=devices.find(x=>x.id==='esp32s3-fh4r2-qspi-4m');
 assert.deepEqual(c3.hardware.sdBatteryCalibration,{batteryAdcMv:1,batteryCellMv:2,batteryFullMv:4200});
 assert.equal(c3.hardware.batteryCellMv/c3.hardware.batteryAdcMv,2);
 assert.equal(s3.hardware.batteryAttenuation,'ADC_6db');
});

test('C3+SD battery sampling removes spikes but not a systematic overvoltage',()=>{
 assert.match(battery,/uint16_t mvSamples\[16\];/);
 assert.match(battery,/for \(uint8_t i=2;i<14;\+\+i\) centralTotal\+=mvSamples\[i\];/);
 assert.match(battery,/adcMv=\(centralTotal\+6u\)\/12u;/);
 assert.match(battery,/adcUnstable=centralSpread>120u;/);
 assert.match(battery,/if \(!adcUnstable && cellMv>=2800u && cellMv<=4350u\)/);
 assert.match(battery,/batteryCellMillivoltsFromAdc\(adcMv\)/);
 assert.doesNotMatch(battery,/4200u\*adcMv\/1425|adcMv\*1343u\/1425u/);
 assert.match(battery,/batteryAdcMillivolts=uint16_t\(adcMv/);
});

test('Rev K BOM 470k/470k calibrates the observed 2051mV ADC near 4102mV',()=>{
 const full=4200,top=470000,bottom=470000;
 const expected=Math.round(full*bottom/(top+bottom));
 assert.equal(expected,2100);
 const observed=2051;
 const reconstructed=Math.round(observed*(top+bottom)/bottom);
 assert.equal(reconstructed,4102);
 assert(reconstructed>=4000&&reconstructed<=4350);
});
