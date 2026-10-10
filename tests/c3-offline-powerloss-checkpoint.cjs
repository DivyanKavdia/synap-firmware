
'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const recorder=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
test('C3 FatFs checkpoints use genuine POSIX fsync, not fflush-only or filesystem remount',()=>{
 const start=recorder.indexOf('static constexpr uint32_t ODYSSEY_SD_CHECKPOINT_INTERVAL_MS');
 const end=recorder.indexOf('static void odysseyRecordTake() {',start);
 assert(start>=0&&end>start);
 const helper=recorder.slice(start,end);
 assert.match(helper,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u/);
 assert.match(helper,/if \(fflush\(file\)!=0\)/);
 assert.match(helper,/const int fd=fileno\(file\)/);
 assert.match(helper,/if \(fsync\(fd\)!=0\)/);
 assert(helper.indexOf('fflush(file)')<helper.indexOf('fsync(fd)'));
 assert.doesNotMatch(helper,/SD\.end\(|SD\.begin\(|ftruncate\(|fseek\(|unlink\(/);
 const output=nativeTest(`
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <iostream>
int mode=0,flushCalls=0,fdCalls=0,syncCalls=0;
int fakeFlush(FILE*) {
 ++flushCalls;
 if(mode==1){errno=EIO;return -1;}
 return 0;
}
int fakeFileno(FILE*) {
 ++fdCalls;
 if(mode==2){errno=EBADF;return -1;}
 return 10;
}
int fakeSync(int fd) {
 assert(fd==10);++syncCalls;
 if(mode==3){errno=ENODEV;return -1;}
 return 0;
}
#define fflush fakeFlush
#define fileno fakeFileno
#define fsync fakeSync
${helper}
#undef fflush
#undef fileno
#undef fsync
int main() {
 assert(ODYSSEY_SD_CHECKPOINT_INTERVAL_MS==10000);
 FILE* f=reinterpret_cast<FILE*>(uintptr_t(1));
 int error=0;
 mode=0;assert(odysseyCheckpointWav(f,error)==0&&error==0);
 assert(flushCalls==1&&fdCalls==1&&syncCalls==1);
 mode=1;assert(odysseyCheckpointWav(f,error)==71&&error==EIO);
 assert(fdCalls==1&&syncCalls==1);
 mode=2;assert(odysseyCheckpointWav(f,error)==73&&error==EBADF);
 assert(syncCalls==1);
 mode=3;assert(odysseyCheckpointWav(f,error)==72&&error==ENODEV);
 assert(syncCalls==2);
 std::cout<<"PASS power-loss checkpoint helper\\n";
}
`);
 assert.match(output,/PASS power-loss checkpoint helper/);
});
test('checkpoint only after complete accepted PCM batch under full SD mutex; no retried writes',()=>{
 const capture=recorder.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
 assert.match(capture,/OdysseySdGuard storage;/);
 const batch=capture.split('if (batchUsed==sizeof(batch)) {')[1].split('// Capture performs only sequential')[0];
 assert(batch.indexOf('const size_t written=fwrite(batch')<batch.indexOf('bytes+=batchPcmBytes'));
 assert(batch.indexOf('bytes+=batchPcmBytes')<batch.indexOf('odysseyCheckpointWav(file,checkpointErrno)'));
 assert.match(batch,/if \(uint32_t\(millis\(\)-lastCheckpointAt\)>=ODYSSEY_SD_CHECKPOINT_INTERVAL_MS\)/);
 assert.match(batch,/if \(checkpointStage\) \{[\s\S]*?failed=true;[\s\S]*?failureStage=checkpointStage;[\s\S]*?break;/);
 assert.match(batch,/odysseyPersistedWriteErrno=uint32_t\(checkpointErrno\)/);
 assert.match(capture,/fclose\(file\)!=0/);
 assert.doesNotMatch(capture,/fseek\(file,0|ftruncate\(|SD\.format\(|unlink\(/);
 assert.match(capture,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
});
test('power-interrupted WAV remains discoverable; download restores virtual RIFF header in RAM',()=>{
 const read=transfer.split('static uint8_t readSelected(')[1].split('static uint8_t catalogue(')[0];
 assert.match(read,/patchVirtualWavHeader\(bytes,size,offset,total\)/);
 const header=transfer.split('static void virtualWavHeader(')[1].split('static void notify',1)[0];
 assert.match(header,/put32le\(h\+40,pcmBytes\)/);
 assert.match(header,/put32le\(h\+4,pcmBytes\+36u\)/);
 assert.match(read,/FILE\* file=fopen\(full,"rb"\)/);
 assert.doesNotMatch(read,/fopen\(full,"wb"\)|ftruncate\(|unlink\(/);
 assert.match(transfer,/if \(stat\(full,&st\)!=0 \|\| !S_ISREG\(st\.st_mode\)/);
});
