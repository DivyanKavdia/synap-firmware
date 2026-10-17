'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('zero PCM cannot reach the saved message as a successful take',()=>{
 const {nativeTest}=require('./support/native.cjs');
 const guard=rec.slice(rec.indexOf('  if (!failed && bytes==0)'),rec.indexOf('  // A successful fwrite/fclose'));
 nativeTest(`#include <cassert>
 int main(){
   bool failed=false; unsigned bytes=0,failureStage=0;
   ${guard}
   assert(failed && failureStage==48);
   failed=true; failureStage=70;
   ${guard}
   assert(failed && failureStage==70);
   failed=false; failureStage=0; bytes=4096;
   ${guard}
   assert(!failed && failureStage==0);
 }`);
});
test('any uncertain PCM/checkpoint write quarantines before close or remount',()=>{
 assert.match(rec,/bool uncertainSdWrite=false/);
 assert.match(rec,/writeError!=ENOSPC\) uncertainSdWrite=true/);
 assert.match(rec,/uncertainSdWrite=true;[\s\S]*?failureStage=checkpointStage/);
 const first=rec.split('if (file && (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite)) {')[1].split('if (file) {')[0];
 assert.match(first,/odysseySdQuarantineUncertainWrite\(\)/);
 assert.match(first,/file=nullptr/);
 assert.doesNotMatch(first,/fclose\(|fsync\(|fflush\(/);
 const last=rec.split('if (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite) {')[1].split('} else {')[0];
 assert.match(last,/odysseySdQuarantineUncertainWrite\(\)/);
 assert.match(last,/file=nullptr/);
 const quarantine=detect.split('void odysseySdQuarantineUncertainWrite() {')[1].split('bool odysseySdWriteTimeoutQuarantined() {')[0];
 assert.match(quarantine,/odysseySdTimedOutWriteQuarantined=true/);
 assert.match(quarantine,/odysseySdUnsafeToSleep=true/);
 assert.match(detect,/same-rail remount refused/);
});
test('one-time mic restart does not retry SD transactions or classify mic as SD',()=>{
 assert.match(rec,/odysseyMicRecoveryAttempts=1/);
 assert.match(rec,/stopMicrophone\(\);\s*if \(startMicrophone\(\)\)/);
 assert.match(rec,/failed=true;failureStage=43/);
 assert.match(rec,/persistedStage==43u\?3u/);
 assert.match(rec,/storageFailure=persistedStage!=43u/);
 assert.match(rec,/odysseyMicRecoveryAttempts=0/);
});
test('disk-full is distinct from card transport failure and remains cataloguable',()=>{
 assert.match(rec,/persistedStage!=52u && persistedStage!=68u/);
 assert.match(detect,/previousRecordStage!=52u &&/);
 assert.match(detect,/previousRecordStage!=68u/);
 assert.match(tx,/recordedFault!=52u && recordedFault!=68u/);
});
test('C3 LEDs distinguish microphone, SD and unsafe card; RAM health never touches FAT',()=>{
 assert.match(ino,/Red pulses identify the cause/);
 assert.match(ino,/odysseyRecorderFaultLedCode\.load\(\)==3u\?3u/);
 assert.match(ino,/odysseyRecorderFaultLedCode\.load\(\)==4u\?4u/);
 const health=tx.split('case 34: {')[1].split('case 29: {')[0];
 for(const word of ['sdState','sdLiveProbe','stage','bytes','faultLed','micRestarts','recording','stopPending','readQuarantine','writeQuarantine'])
   assert(health.includes(word),word);
 assert.doesNotMatch(health,/fopen\(|fwrite\(|fclose\(|odysseyRecoverSdCard\(|odysseyFormatSdCard\(|odysseySdBeginLocked\(/);
 assert.match(health,/reply\(request,OK/);
});
