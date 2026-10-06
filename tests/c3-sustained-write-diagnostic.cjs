'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');

test('C3 offline double tap uses the historical 1631 recorder without a diagnostic write preflight',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  assert.doesNotMatch(source,/odysseySustainedWriteProbe|\.synap-sustained-write\.tmp|bytes=64000/);
  const take=source.split('static void odysseyRecordTake() {')[1];
  assert.match(take,/OdysseySdGuard storage/);
  assert.match(take,/file=fopen\(fullPath,"wb\+"\)/);
  assert.match(take,/startMicrophone\(\)/);
  assert(take.indexOf('file=fopen(fullPath,"wb+")')<take.indexOf('startMicrophone()'));
});

test('C3 recording diagnostics retain only completed historical recorder failure stages',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  for(const stage of [40,41,42,43,44,45,46,47,48])
    assert.match(source,new RegExp('failureStage='+stage));
  for(const stage of [50,51,52,53,54])
    assert.doesNotMatch(source,new RegExp('failureStage='+stage));
  assert.match(source,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(source,/odysseyPersistRecordFailure\(0,0\)/);
});

test('C3 first completed record failure survives reboot without changing the live mount byte',()=>{
  const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(recorder,/prefs\.begin\("sd-recdiag",false\)/);
  assert.match(caps,/p\[18\]=odysseySdDetectionState\(\)/);
  assert.match(caps,/p\[15\]=lastRecordStage/);
  assert.match(caps,/p\[19\]=lastRecordStage\?lastRecordStage:odysseySdProbeState\(\)/);
});
