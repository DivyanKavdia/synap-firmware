'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');

test('C3 offline recorder creates directly with the proven boot-probe write mode',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  assert.doesNotMatch(source,/odysseySustainedWriteProbe|\.synap-sustained-write\.tmp|bytes=64000/);
  const take=source.split('static void odysseyRecordTake() {')[1];
  assert.match(take,/OdysseySdGuard storage/);
  assert.match(take,/odysseySdReserveRecordingFile\(fullPath\)/);
  assert.match(take,/file=fopen\(fullPath,"r\+b"\)/);
  assert.doesNotMatch(take,/stat\(fullPath/);
  assert.match(take,/startMicrophone\(\)/);
  assert(take.indexOf('odysseySdReserveRecordingFile(fullPath)')<take.indexOf('startMicrophone()'));
  assert(take.indexOf('file=fopen(fullPath,"r+b")')<take.indexOf('startMicrophone()'));
  assert.doesNotMatch(take,/checkpointAt|odysseyCheckpointWav/);
  assert.match(source,/odysseyFinalizeWav/);
  assert.match(take,/alignas\(4\) uint8_t sector\[512\]/);
  assert.match(take,/setvbuf\(file,nullptr,_IONBF,0\)/);
  assert.match(take,/fwrite\(sector,1,sizeof\(sector\),file\)/);
});

test('C3 recording diagnostics distinguish create failures from later recorder stages',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  for(const stage of [40,41,42,43,46,47,48])
    assert.match(source,new RegExp('failureStage='+stage));
  assert.doesNotMatch(source,/failureStage=45/);
  for(const stage of [49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64])
    assert.match(source,new RegExp('return '+stage));
  assert.match(source,/failureStage=65/);
  assert.match(source,/case EIO: return 49/);
  assert.match(source,/case ENODEV: return 50/);
  assert.match(source,/case EMFILE:[\s\S]*case ENFILE: return 51/);
  assert.match(source,/case ENOSPC: return 52/);
  assert.match(source,/case EROFS: return 53/);
  assert.match(source,/case EIO: return 60/);
  assert.match(source,/case ENOSPC: return 62/);
  assert.match(source,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(source,/odysseyPersistRecordFailure\(0,0\)/);
});

test('C3 first completed record failure survives reboot without changing the live mount byte',()=>{
  const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(recorder,/prefs\.begin\("sd-recdiag",false\)/);
  assert.match(caps,/p\[18\]=odysseySdDetectionState\(\)/);
  assert.match(caps,/p\[15\]=uint8_t\(recordUnits>255u\?255u:recordUnits\)/);
  assert.match(caps,/p\[19\]=lastRecordStage\?lastRecordStage:odysseySdProbeState\(\)/);
});
