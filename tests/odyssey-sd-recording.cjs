'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('C3 local WAV preserves PCM, destination and final header across stop, reconnect and failures',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-recording.cpp','utf8');
 const fixture=fs.readFileSync('tests/odyssey-sd-recording.cpp','utf8');
 if(!/fwrite\(header,1,sizeof\(header\),file\).*fflush\(file\)/s.test(source))
   throw Error('C3 recorder must flush the initial WAV header before microphone capture');
 if(!/file bytes=%ld/.test(source))
   throw Error('C3 recorder must verify and log the finalized file size');
 nativeTest(fixture.replace('// INSERT RECORDER',source),['-DCONFIG_IDF_TARGET_ESP32C3=1','-DUSE_REAL_I2S_MIC=1']);
});
