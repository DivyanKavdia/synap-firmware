'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
const source=()=>fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');

test('exact C3 and S3 capture preserves all PCM values without filtering, including partial reads and recovery',()=>{
  for(const prepared of [source(),materialize(source(),'esp32c3-supermini-4m')]){
    const start=prepared.indexOf('bool acquireAudioFrame(AudioFrame& frame) {');
    const end=prepared.indexOf('void acquisitionTask(void* parameter) {',start);
    const fixture=fs.readFileSync(path.join(__dirname,'audio-capture.cpp'),'utf8');
    const result=nativeTest(fixture.replace('// INSERT PRODUCTION ACQUIRE',prepared.slice(start,end)));
    assert.match(result,/PASS: exact production capture preserves all 65536 PCM values/);
  }
});

test('unfiltered capture retains the proven transport, recovery and touch contracts on both boards',()=>{
  for(const prepared of [source(),materialize(source(),'esp32c3-supermini-4m')]){
    assert.match(prepared,/const int32_t sample=raw\[i\] >> 16;/);
    assert.match(prepared,/frame.samples\[i\]=static_cast<int16_t>\(sample\);/);
    assert.doesNotMatch(prepared,/SpeechHighPass|SYNAP_MIC_HPF_ENABLE|highPass/);
    assert.match(prepared,/if \(startMicrophone\(\)\) \{ received=0; emptyReads=0; continue; \}/);
    assert.match(prepared,/ADPCM_BYTES_PER_FRAME == 404/);
    assert.match(prepared,/TOUCH_SLEEP_HOLD_MS = 4000/);
    assert.match(prepared,/double tap -> STOP \+ POWER SAVER/);
    assert.match(prepared,/writeDurableSleepLock\(true\)/);
    assert.match(prepared,/eventCharacteristic->notify\(\)/);
  }
});
