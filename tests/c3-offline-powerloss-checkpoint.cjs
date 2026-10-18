
'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const recorder=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
test('C3 does not issue mid-record FAT metadata checkpoints after observed CMD24 timeout',()=>{
 const capture=recorder.split('static void odysseyRecordTake() {')[1]
   .split('static void odysseyRecordTask(void*) {')[0];
 assert.match(capture,/ODYSSEY_SD_RECORD_BATCH_BYTES/);
 assert.match(capture,/fwrite\\(batch,1,sizeof\\(batch\\),file\\)/);
 assert.match(capture,/fclose\\(file\\)!=0/);
 assert.match(capture,/odysseySdWriteTimeoutQuarantined\\(\\)/);
 assert.match(capture,/odysseySdQuarantineUncertainWrite\\(\\)/);
 assert.doesNotMatch(recorder,/ODYSSEY_SD_FIRST_CHECKPOINT_PCM_BYTES|ODYSSEY_SD_CHECKPOINT_INTERVAL_MS|odysseyCheckpointWav|\\bfsync\\(/);
 const active=capture.split('while (!failed && !odysseyStopRequested.load()) {')[1]
   .split('stopMicrophone();')[0];
 assert.doesNotMatch(active,/fflush\\(|fclose\\(|fsync\\(|fseek\\(|unlink\\(/);
});
test('power-interrupted WAV remains discoverable; download restores virtual RIFF header in RAM',()=>{
 const read=transfer.split('static uint8_t readWavChunk(')[1].split('static uint8_t readSelected(')[0];
 assert.match(read,/patchVirtualWavHeader\(bytes,size,offset,total\)/);
 const header=transfer.split('static void virtualWavHeader(')[1].split('static void notify',1)[0];
 assert.match(header,/put32le\(h\+40,pcmBytes\)/);
 assert.match(header,/put32le\(h\+4,pcmBytes\+36u\)/);
 assert.match(read,/FILE\* file=fopen\(full,"rb"\)/);
 assert.match(read,/fclose\(file\)/);
 assert.match(read,/sdBleReadCache\.valid=true/);
 assert.doesNotMatch(read,/fopen\(full,"wb"\)|ftruncate\(|unlink\(/);
 assert.match(read,/if \(stat\(full,&st\)!=0\)/);
 assert.match(read,/!S_ISREG\(st\.st_mode\)/);
});
