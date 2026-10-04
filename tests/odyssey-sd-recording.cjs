'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('C3 local WAV preserves PCM, destination and final header across stop, reconnect and failures',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-recording.cpp','utf8');
 const fixture=fs.readFileSync('tests/odyssey-sd-recording.cpp','utf8');
 nativeTest(fixture.replace('// INSERT RECORDER',source),['-DCONFIG_IDF_TARGET_ESP32C3=1','-DUSE_REAL_I2S_MIC=1']);
});

test('C3 offline recorder classifies storage failures separately from microphone faults',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-recording.cpp','utf8');
 if(!/static bool odysseyRecordTake\(\)/.test(source)) throw Error('recorder must report storage-fault class to its task');
 if(!/storageFault=true;odysseySdMarkVfsFailure\(\)/.test(source)) throw Error('storage write/finalize failures must downgrade VFS immediately');
 if(!/odysseySdQuiesceFaultedSession\(750u\)[\s\S]*odysseySdRequestRecovery\(\)/.test(source))
   throw Error('offline storage faults must be cleaned and rearmed after file ownership ends');
});
