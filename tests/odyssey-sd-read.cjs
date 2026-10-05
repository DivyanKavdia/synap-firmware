'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('C3 reads cache descriptors by connection and path, support retries, close at EOF and reject unfinished WAV',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-transfer.cpp','utf8');
 const io=fs.readFileSync('firmware/shared/odyssey-sd-io.cpp','utf8');
 const fixture=fs.readFileSync('tests/odyssey-sd-read.cpp','utf8');
 nativeTest(fixture.replace('// INSERT IO',io)
   .replace('// INSERT PATHS',source.slice(source.indexOf('static bool safeWavPath'),source.indexOf('static void reply')))
   .replace('// INSERT READ',source.slice(source.indexOf('static uint8_t readSelected'),source.indexOf('static uint8_t catalogue'))),
   ['-DCONFIG_IDF_TARGET_ESP32C3=1']);
});
