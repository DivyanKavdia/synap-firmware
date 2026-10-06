'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');

test('C3 sustained-write diagnostic runs before microphone capture using 1631 frame size',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  assert.match(source,/static bool odysseySustainedWriteProbe\(uint8_t& failureStage\)/);
  assert.match(source,/uint8_t block\[AUDIO_BYTES_PER_FRAME\]/);
  assert.match(source,/for \(uint8_t frame=0;frame<40u;\+\+frame\)/);
  assert.match(source,/fwrite\(block,1,sizeof\(block\),probe\)!=sizeof\(block\)/);
  assert.match(source,/fflush\(probe\)!=0/);
  const take=source.split('static void odysseyRecordTake() {')[1];
  assert(take.indexOf('odysseySustainedWriteProbe(failureStage)')<take.indexOf('startMicrophone()'));
});

test('C3 write diagnostic distinguishes preflight and historical recorder stages',()=>{
  const source=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  for(const stage of [40,41,42,43,44,45,46,47,48,50,51,52,53,54])
    assert.match(source,new RegExp('failureStage='+stage));
});

test('C3 first record failure survives reboot without changing the live mount byte',()=>{
  const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(recorder,/prefs\.begin\("sd-recdiag",false\)/);
  assert.match(recorder,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(recorder,/odysseyPersistRecordFailure\(0,0\)/);
  assert.match(caps,/p\[18\]=odysseySdDetectionState\(\)/);
  assert.match(caps,/p\[15\]=lastRecordStage/);
  assert.match(caps,/p\[19\]=lastRecordStage\?lastRecordStage:odysseySdProbeState\(\)/);
});
