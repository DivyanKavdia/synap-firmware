'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const {patch}=require('../tools/patch-c3-sd-write.cjs');
test('clean C3 backend rejects unaccepted physical-sector writes and retries CRC errors',()=>{
 const original=fs.readFileSync('tests/fixtures/arduino-sd-write-3.3.5.cpp','utf8');
 const fixed=patch(original);
 assert.equal(patch(fixed),fixed);assert.throws(()=>patch('unexpected SDK'),/shape changed/);
 nativeTest(fs.readFileSync('tests/c3-clean-sd-write.cpp','utf8').replace('// INSERT WRITE',fixed),['-DCONFIG_IDF_TARGET_ESP32C3=1','-funsigned-char']);
});
