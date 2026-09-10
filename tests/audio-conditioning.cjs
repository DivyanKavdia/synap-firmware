'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{execFileSync}=require('node:child_process');
const {prepareProduction,removeUnusedInteractionState}=require('../tools/prepare-production.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
const source=()=>prepareProduction(fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8'));

function nativeTest(body,flags=[]){
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'synap-audio-'));
  try{
    const file=path.join(dir,'test.cpp'),binary=path.join(dir,'test');
    fs.writeFileSync(file,body);
    execFileSync('g++',['-std=c++17','-Wall','-Wextra','-Werror','-O2','-fsanitize=undefined','-fno-sanitize-recover=all',...flags,file,'-o',binary]);
    return execFileSync(binary,{encoding:'utf8',timeout:20000});
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
}

test('production DSP has measured rumble rejection, speech-band preservation and saturating PCM',()=>{
  const prepared=source();
  const embedded=prepared.slice(prepared.indexOf('#ifndef SYNAP_AUDIO_CONDITIONING_H'),prepared.indexOf('static_assert(SAMPLE_RATE==16000'));
  const fixture=fs.readFileSync(path.join(__dirname,'audio-conditioning.cpp'),'utf8');
  const result=nativeTest(embedded+'\n'+fixture);
  assert.match(result,/PASS: filter bytes=12/);
  console.log(result.trim());
});

test('comparison build bypass preserves all 65536 PCM values exactly',()=>{
  const header=fs.readFileSync(path.join(root,'tools/audio-conditioning.h'),'utf8');
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

test('unused-state cleanup refuses to remove a symbol that becomes live',()=>{
  const declarations=`constexpr uint16_t TOUCH_DOUBLE_TAP_MS = 500;
constexpr uint16_t TOUCH_LONG_PRESS_MS = 1200;
constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 5000;
bool touchRawState = false, touchStableState = false, touchLongSent = false, touchLongEligible = false, touchIdlePress = false;
uint32_t touchFirstTapAt = 0, touchPressedAt = 0;
float tonePhase = 0;
`;
  assert.doesNotMatch(removeUnusedInteractionState(declarations),/touchLongSent/);
  assert.throws(()=>removeUnusedInteractionState(declarations+'\nif(touchLongSent) doSomething();'),/Expected unused declaration only: touchLongSent/);
  assert.throws(()=>removeUnusedInteractionState(declarations+'\nif(touchFirstTapAt) doSomething();'),/Legacy first-tap timestamp became live/);
});

module.exports={nativeTest};
