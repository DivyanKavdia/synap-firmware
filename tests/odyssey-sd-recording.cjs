'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('C3 local WAV preserves PCM, destination and final header across stop, reconnect and failures',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-recording.cpp','utf8');
 const fixture=fs.readFileSync('tests/odyssey-sd-recording.cpp','utf8');
 assert.match(source,/ODYSSEY_SD_WRITE_BUFFER_BYTES=12800u/);
 assert.match(source,/ODYSSEY_WAV_CHECKPOINT_MS=15000u/);
 assert.match(source,/odysseyFlushPcmBuffer/);
 assert.match(source,/malloc\(ODYSSEY_SD_WRITE_BUFFER_BYTES\)/);
 assert.doesNotMatch(source,/checkpointAt\)>=2000u/);
 nativeTest(fixture.replace('// INSERT RECORDER',source),['-DCONFIG_IDF_TARGET_ESP32C3=1','-DUSE_REAL_I2S_MIC=1']);
});
