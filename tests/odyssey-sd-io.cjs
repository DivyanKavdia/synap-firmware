'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('C3 journal recovers torn header/commit, preserves invalid journals, handles EINTR/short writes and sync failures',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-io.cpp','utf8');
 const fixture=fs.readFileSync('tests/odyssey-sd-io.cpp','utf8');
 nativeTest(fixture.replace('// INSERT IO',source),['-DCONFIG_IDF_TARGET_ESP32C3=1']);
});
