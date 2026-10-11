'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');

test('C3 offline recorder is append-only and writes aligned multi-sector batches',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  assert.doesNotMatch(source,/odysseySustainedWriteProbe|\.synap-sustained-write\.tmp|bytes=64000/);
  const take=source.split('static void odysseyRecordTake() {')[1];
  assert.match(take,/OdysseySdGuard storage/);
  assert.match(take,/file=fopen\(fullPath,"wb"\)/);
  assert.doesNotMatch(take,/odysseySdReserveRecordingFile|esp_vfs_fat_create_contiguous_file|ftruncate\(|odysseyFinalizeWav|fseek\(file,0/);
  // No random seeks/stat checks during the write session: only after close.
  assert(take.indexOf('stat(fullPath')>take.indexOf('if (fclose(file)!=0)'));
  assert.match(take,/fopen\(fullPath,"rb"\)/);
  assert.match(take,/failureStage=74/);
  assert.match(take,/failureStage=75/);
  assert.match(take,/startMicrophone\(\)/);
  assert(take.indexOf('file=fopen(fullPath,"wb")')<take.indexOf('startMicrophone()'));
  assert.match(take,/odysseyCheckpointWav\(file,checkpointErrno\)/);
  assert.match(take,/uint32_t\(millis\(\)-lastCheckpointAt\)>=ODYSSEY_SD_CHECKPOINT_INTERVAL_MS/);
  assert.doesNotMatch(take,/fseek\(file,0|ftruncate\(|SD\.format\(/);
  assert.match(take,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(take,/setvbuf\(file,nullptr,_IONBF,0\)/);
  assert.match(take,/fwrite\(batch,1,sizeof\(batch\),file\)/);
  assert.match(take,/memset\(batch\+batchUsed,0,sizeof\(batch\)-batchUsed\)/);
  assert.match(take,/fclose\(file\)/);
  assert.doesNotMatch(take,/uint8_t sector\[512\]|fwrite\(sector/);
});

test('C3 recording diagnostics distinguish create, batch-write, close and zero-audio failures',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  for(const stage of [40,41,42,43,47,48])
    assert.match(source,new RegExp('failureStage='+stage));
  for(const stage of [45,46,60,61,62,63,64,65])
    assert.doesNotMatch(source,new RegExp('failureStage='+stage+'|return '+stage));
  for(const stage of [49,50,51,52,53,54,66,67,68,69,70])
    assert.match(source,new RegExp('return '+stage));
  assert.match(source,/case EIO: return 49/);
  assert.match(source,/case ENODEV: return 50/);
  assert.match(source,/case EMFILE:[\s\S]*case ENFILE: return 51/);
  assert.match(source,/case ENOSPC: return 52/);
  assert.match(source,/case EROFS: return 53/);
  assert.match(source,/case EIO: return 66/);
  assert.match(source,/case ENOSPC: return 68/);
  assert.match(source,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(source,/odysseyPersistRecordFailure\(0,0\)/);
});

test('C3 stale write failures are re-armed before boot mount and before the next take',()=>{
  const detect=read('firmware/shared/odyssey-sd-1631-detect.cpp');
  const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(detect,/previousRecordStage>=44u/);
  assert.match(detect,/previousRecordStage!=48u/);
  assert.match(detect,/odysseySdBitBangRecoverLocked\("rearm"\)/);
  assert.match(recorder,/one-gesture offline start: recovering storage before capture/);
  assert.match(recorder,/odysseyRecoverSdCard\("touch"\)/);
  assert.match(recorder,/skipping automatic remount/);
  const afterTake=recorder.split('  odysseyRecordTake();')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.doesNotMatch(afterTake,/odysseyRecoverSdCard\("rearm"\)/);
  assert.doesNotMatch(recorder,/Retry double tap after mount/);
  assert.match(caps,/liveProbe<=6u\?liveProbe:7u/);
  assert.match(caps,/<<3/);
});

test('C3 first completed record failure survives reboot without changing the live mount byte',()=>{
  const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(recorder,/prefs\.begin\("sd-recdiag",false\)/);
  assert.match(caps,/p\[18\]=odysseySdDetectionState\(\)/);
  assert.match(caps,/p\[15\]=uint8_t\(recordUnits>255u\?255u:recordUnits\)/);
  assert.match(caps,/p\[16\]\|=0x80/);
  assert.match(caps,/p\[19\]=lastRecordStage\?lastRecordStage:liveProbe/);
});


test('C3 transfer synthesizes a valid virtual WAV header for append-only files',()=>{
  const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
  assert.match(transfer,/static void virtualWavHeader/);
  assert.match(transfer,/pcmBytes=totalBytes>44u\?totalBytes-44u:0u/);
  assert.match(transfer,/patchVirtualWavHeader\(bytes,size,offset,total\)/);
});

test('C3 FAT mkdir and WAV fopen errors preserve the FIRST core SD write fault',()=>{
 const rec=read('firmware/shared/odyssey-sd-1631-recording.cpp');
 assert.match(rec,/odysseyCaptureCreateFault\(savedErrno,"mkdir \/synap",failureStage\)/);
 assert.match(rec,/failureStage=directoryErrno==ENOENT\?76u:77u/);
 assert.match(rec,/failureStage=78/);
 assert.match(rec,/odysseyCaptureCreateFault\(openError,"fopen WAV",failureStage\)/);
 assert.match(rec,/odysseyPersistedDriverFault=synapSdWriteFaultCode\(\)/);
 assert.match(rec,/odysseyPersistedWriteErrno=err>0/);
 assert.match(rec,/odysseyCaptureCreateFault\(closeErrno,"fclose WAV",failureStage\)/);
 assert.match(rec,/synapSdClearWriteFaultCode\(\)/);
 const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
 const report=transfer.split('case 27: {')[1].split('case 4:')[0];
 assert.match(report,/odysseyLastRecordFailureStage\(\)/);
 assert.match(report,/odysseyLastDriverWriteFault\(\)/);
 assert.match(report,/reply\(request,OK/);
 const executable=report.replace(/\/\/[^\n]*/g,''); // comments may name forbidden operations
 assert.doesNotMatch(executable,/SD\.begin|SD\.end|mkdir\(|fopen\(|unlink\(|odysseyRecoverSdCard/);
});
