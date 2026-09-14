'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']) {
  test(target+': production capture preserves every PCM16 value through partial reads and recovery',()=>{
    const prepared=materialize(source,target);
    const capture=prepared.slice(prepared.indexOf('bool acquireAudioFrame(AudioFrame& frame) {'),prepared.indexOf('void acquisitionTask(void* parameter) {'));
    const fixture=fs.readFileSync(path.join(__dirname,'audio-capture.cpp'),'utf8');
    assert.match(nativeTest(fixture.replace('// INSERT PRODUCTION ACQUIRE',capture)),/PASS: exact production capture/);
  });
}
