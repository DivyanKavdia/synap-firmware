'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {prepareProduction}=require('../tools/prepare-production.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
const source=()=>prepareProduction(fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8'));

const {nativeTest}=require('./support/native.cjs');

test('production DSP has measured rumble rejection, speech-band preservation and saturating PCM',()=>{
  const prepared=source();
  const embedded=prepared.slice(prepared.indexOf('#ifndef SYNAP_AUDIO_CONDITIONING_H'),prepared.indexOf('static_assert(SAMPLE_RATE==16000'));
  const fixture=fs.readFileSync(path.join(__dirname,'audio-conditioning.cpp'),'utf8');
  const result=nativeTest(embedded+'\n'+fixture);
  assert.match(result,/PASS: filter bytes=12/);
  console.log(result.trim());
});

test('comparison build bypass preserves all 65536 PCM values exactly',()=>{
  const header='#ifndef SYNAP_AUDIO_CONDITIONING_H'+source().split('#ifndef SYNAP_AUDIO_CONDITIONING_H')[1].split('static_assert(SAMPLE_RATE==16000')[0];
  const fixture=fs.readFileSync(path.join(__dirname,'audio-conditioning.cpp'),'utf8');
  assert.match(nativeTest(header+'\n'+fixture,['-DSYNAP_MIC_HPF_ENABLE=0']),/PASS: bypass/);
});

test('exact production capture handles partial reads, frame boundaries, recording changes and I2S recovery',()=>{
  const prepared=source();
  const start=prepared.indexOf('#ifndef SYNAP_AUDIO_CONDITIONING_H');
  const end=prepared.indexOf('void acquisitionTask(void* parameter) {',start);
  const capture=prepared.slice(start,end).replace('static_assert(SAMPLE_RATE==16000, "Speech high-pass requires 16 kHz PCM");','');
  const fixture=fs.readFileSync(path.join(__dirname,'audio-capture.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT PRODUCTION ACQUIRE',capture)),/PASS: exact production capture/);
});

test('both final targets share capture-owned filter history and retain transport and gestures',()=>{
  for(const prepared of [source(),materialize(source(),'esp32c3-supermini-4m')]){
    assert.match(prepared,/const int32_t sample=raw\[i\] >> 16;\s*frame.samples\[i\]=highPass.process/);
    assert.match(prepared,/if \(highPassGeneration!=frame.generation\) \{\s*highPass.reset\(\)/);
    assert.match(prepared,/if \(startMicrophone\(\)\) \{ highPass.reset\(\); received=0; emptyReads=0; continue; \}/);
    assert.match(prepared,/ADPCM_BYTES_PER_FRAME == 404/);
    assert.match(prepared,/triple tap -> DEEP SLEEP/);
    assert.match(prepared,/double tap -> STOP \+ POWER SAVER/);
    assert.match(prepared,/writeDurableSleepLock\(true\)/);
    assert.doesNotMatch(prepared,/\b(?:TOUCH_DOUBLE_TAP_MS|TOUCH_LONG_PRESS_MS|TOUCH_SLEEP_HOLD_MS|touchLongSent|touchLongEligible|touchIdlePress|touchFirstTapAt|publishRememberEvent|memoryAckUntil|memoryEventCounter|streamStartedAt)\b/);
    assert.match(prepared,/eventCharacteristic->notify\(\)/);
    assert.match(prepared,/#if !USE_REAL_I2S_MIC\nfloat tonePhase = 0;\n#endif/);
  }
});
