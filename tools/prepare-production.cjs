'use strict';
const fs=require('node:fs'),path=require('node:path');
const {replaceOnce}=require('./patch-production-hardening.cjs');
const {replaceFunction}=require('./patch-power-failclosed.cjs');

// Keep the legacy transformations while they are still required by the base
// sketch. This is the single ordered production pipeline used by CI and tests.
const stages=[
  require('./prepare-interactions.cjs').prepare,
  require('./patch-runtime-fixes.cjs').patch,
  require('./patch-event-channel.cjs').patch,
  require('./patch-battery-v2.cjs').patch,
  require('./patch-audio-reliability.cjs').patch,
  require('./patch-audio-codec-v3.cjs').patch,
  require('./patch-touch-reliability.cjs').patch,
  require('./patch-production-hardening.cjs').patch,
  require('./patch-power-controls-v2.cjs').patch,
  require('./patch-power-failclosed.cjs').patch,
];

function conditionAudio(source){
  const header=fs.readFileSync(path.join(__dirname,'audio-conditioning.h'),'utf8');
  let out=replaceOnce(source,'bool acquireAudioFrame(AudioFrame& frame) {',
    header+'\nstatic_assert(SAMPLE_RATE==16000, "Speech high-pass requires 16 kHz PCM");\n\nbool acquireAudioFrame(AudioFrame& frame) {',
    'capture conditioning implementation');
  out=replaceOnce(out,'  static int32_t raw[SAMPLES_PER_FRAME];',
`  static int32_t raw[SAMPLES_PER_FRAME];
  // Capture-task ownership avoids sharing filter state with the control task.
  static SynapAudio::SpeechHighPass highPass;
  static uint32_t highPassGeneration=0;
  if (highPassGeneration!=frame.generation) {
    highPass.reset();
    highPassGeneration=frame.generation;
  }`,'capture filter state');
  out=replaceOnce(out,'if (startMicrophone()) { received=0; emptyReads=0; continue; }',
    'if (startMicrophone()) { highPass.reset(); received=0; emptyReads=0; continue; }',
    'driver recovery clears old filter history');
  out=replaceOnce(out,'    frame.samples[i]=static_cast<int16_t>(sample);',
    '    frame.samples[i]=highPass.process(static_cast<int16_t>(sample));',
    'condition unity-gain PCM before ADPCM');
  return out;
}

function removeUnusedInteractionState(source){
  let out=source;
  // The current triple/double-tap state machine never reads these legacy hold
  // constants/flags. Require the exact declaration-only occurrence before removal.
  for(const symbol of ['TOUCH_DOUBLE_TAP_MS','TOUCH_LONG_PRESS_MS','TOUCH_SLEEP_HOLD_MS',
    'touchLongSent','touchLongEligible','touchIdlePress']){
    const matches=out.match(new RegExp('\\b'+symbol+'\\b','g'))||[];
    if(matches.length!==1)throw Error(`Expected unused declaration only: ${symbol}`);
  }
  out=out.replace(/^constexpr uint16_t TOUCH_(?:DOUBLE_TAP|LONG_PRESS|SLEEP_HOLD)_MS = \d+;\n/gm,'');
  out=replaceOnce(out,
    'bool touchRawState = false, touchStableState = false, touchLongSent = false, touchLongEligible = false, touchIdlePress = false;',
    'bool touchRawState = false, touchStableState = false;','unused legacy hold flags');
  // This old first-tap timestamp is now only initialized/reset; the active state
  // machine uses tapCount/tapSequenceStartedAt/lastTapAt instead.
  const remaining=out.replace(/\btouchFirstTapAt\s*=\s*0\s*[,;]/g,'');
  if(/\btouchFirstTapAt\b/.test(remaining))throw Error('Legacy first-tap timestamp became live');
  out=out.replace(/touchFirstTapAt = 0, /g,'')
    .replace(/^[ \t]*touchFirstTapAt=0;\n/gm,'').replace(/touchFirstTapAt=0;/g,'');
  // The debug oscillator is still useful for transport diagnostics, but its
  // state does not belong in a production microphone build.
  out=replaceOnce(out,'float tonePhase = 0;',
    '#if !USE_REAL_I2S_MIC\nfloat tonePhase = 0;\n#endif','debug-only oscillator state');
  return out;
}

function removeUnreachableRememberPublisher(source){
  // The final gesture state machine and command switch have no Remember action.
  // Keep the event characteristic itself: battery/power notifications still use it.
  if ((source.match(/\bpublishRememberEvent\b/g)||[]).length!==2)
    throw Error('Remember publisher acquired a caller; review before removing');
  let out=replaceFunction(source,'void publishRememberEvent()','','unreachable Remember publisher');
  out=replaceOnce(out,'void publishRememberEvent();\n','','unreachable Remember prototype');
  out=replaceOnce(out,
`  if (memoryAckUntil && static_cast<int32_t>(memoryAckUntil-now)>0) {
    const uint32_t phase=(memoryAckUntil-now)%240u;
    if (phase>120u) { g=LED_DIM+2; b=LED_DIM+2; }
  } else if (otaBusy()) {`,
`  if (otaBusy()) {`,'unreachable Remember LED acknowledgement');
  for(const symbol of ['MEMORY_EVENT_MAGIC','MEMORY_EVENT_VERSION','MEMORY_EVENT_REMEMBER','memoryAckUntil','memoryEventCounter']){
    if ((out.match(new RegExp('\\b'+symbol+'\\b','g'))||[]).length!==1)
      throw Error(`Remember state is still live: ${symbol}`);
  }
  out=out.replace(/^constexpr uint8_t MEMORY_EVENT_(?:MAGIC|VERSION|REMEMBER) = (?:0x[0-9A-Fa-f]+|\d+);\n/gm,'');
  out=replaceOnce(out,'uint32_t touchPressedAt = 0, memoryAckUntil = 0, memoryEventCounter = 0;',
    'uint32_t touchPressedAt = 0;','unused Remember state');
  const withoutTimestampWrites=out.replace(/\bstreamStartedAt\s*=\s*(?:0|millis\(\))\s*;/g,'');
  if (/\bstreamStartedAt\b/.test(withoutTimestampWrites))throw Error('Remember timestamp acquired a reader');
  out=out.replace(/^uint32_t streamStartedAt = 0;\n/m,'')
    .replace(/^[ \t]*streamStartedAt=(?:0|millis\(\));\n/gm,'');
  return out;
}

function prepareProduction(source){
  for(const stage of stages)source=stage(source);
  return removeUnreachableRememberPublisher(removeUnusedInteractionState(conditionAudio(source)));
}

if(require.main===module){
  const [input,output]=process.argv.slice(2);
  if(!input || !output)throw Error('Usage: node tools/prepare-production.cjs <base-sketch> <output-sketch>');
  const prepared=prepareProduction(fs.readFileSync(input,'utf8'));
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,prepared);
  console.log('Prepared complete Synap production source with speech rumble conditioning');
}
module.exports={prepareProduction,conditionAudio,removeUnusedInteractionState,removeUnreachableRememberPublisher};
