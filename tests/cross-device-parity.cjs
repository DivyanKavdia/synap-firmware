'use strict';
// Reference contract: Odyssey C3+SD. Enforce common behavior across every
// shipped partition track without pretending cardless or camera boards are identical.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {TARGETS,capabilityMask,FLAGS}=require('../tools/targets.cjs');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const sketch=assemble();
const ids=Object.keys(TARGETS);
const c3=['esp32c3-supermini-4m','esp32c3-supermini-4m-sd','esp32c3-supermini-4m-sd-ota'];
test('all current production variants retain the canonical BLE, touch, battery, sleep and OTA contracts',()=>{
 assert.equal(ids.length,5,'five OTA/partition tracks are production');
 for(const id of ids){
  const target=TARGETS[id],src=materialize(sketch,id);
  assert(src.includes('#define SYNAP_MODULE_ID '+target.moduleId),id+' module');
  assert(src.includes('constexpr uint8_t RGB_LED_PIN = '+target.hardware.led+';'),id+' LED');
  assert(src.includes('bool startMicrophone()'),id+' microphone');
  assert(src.includes('bool acquireAudioFrame(AudioFrame& frame)'),id+' audio');
  assert(src.includes('void transmitterTask(void* parameter)'),id+' transport');
  assert(src.includes('void processCommand(uint8_t command, uint8_t version)'),id+' control');
  assert(src.includes('void pollTouchControl()'),id+' touch');
  assert(src.includes('TOUCH_DOUBLE_TAP_GAP_MS = 550'),id+' double tap');
  assert(src.includes('TOUCH_SLEEP_HOLD_MS = 4000'),id+' sleep hold');
  assert(src.includes('void enterDeepSleep(const char* reason)'),id+' deep sleep');
  assert(src.includes('void encodeModuleCapabilities(uint8_t* p)'),id+' capabilities');
  assert(src.includes('void otaTick()'),id+' OTA');
  assert(src.includes('sampleBattery(true)'),id+' battery');
  assert(src.includes(target.productMarker),id+' image marker');
  assert(src.includes('"SYNAP-FW:'+(target.firmwareTarget||id)+':"'),id+' identity');
  assert(src.includes('SAMPLE_RATE = 16000'),id+' 16 kHz');
  assert.equal(capabilityMask(target)&(FLAGS.audio|FLAGS.touch|FLAGS.battery|FLAGS.standby),
   FLAGS.audio|FLAGS.touch|FLAGS.battery|FLAGS.standby,id+' shared mask');
 }
});
test('all C3 OTA tracks preserve one shared SD implementation and exactly one hardware pin contract',()=>{
 const cfg=c3.map(id=>TARGETS[id]);
 assert(cfg.every(t=>t.moduleId===2&&t.family==='esp32c3'));
 assert.deepEqual(cfg.map(t=>t.hardware.sdDetection),Array(3).fill({cs:0,sck:10,mosi:21,miso:20}));
 assert.deepEqual(cfg.map(t=>t.hardware.touch),[3,3,3]);
 const sources=c3.map(id=>materialize(sketch,id));
 for(const src of sources){
  assert(src.includes('static void odysseyRecordTask(void*)'), 'offline recorder');
  assert(src.includes('void odysseyToggleRecording()'), 'physical SD toggle');
  assert(src.includes('odysseyPrepareForConnectedStreaming'), 'PWA handoff');
  assert(src.includes('odysseySdQuarantineUncertainWrite'), 'timeout guard');
  assert(src.includes('ODYSSEY_C3_SD_BLE_SINGLE_DELETE_ENABLED=false'), 'preserve SD originals');
  assert(src.includes('virtualWavHeader'), 'verified WAV import compatibility');
  assert(src.includes('esp_vfs_fat_sdspi_mount'), 'native SD driver');
  assert(src.includes('xTaskCreate(odysseyTouchTask'), 'independent physical stop task');
 }
 assert.equal(cfg[0].slotSize,1310720);
 assert.equal(cfg[1].slotSize,1966080);
 assert.equal(cfg[2].slotSize,1966080);
 assert.equal(cfg[2].firmwareTarget,cfg[0].id,'large-slot legacy identity stays stable');
 assert.notEqual(cfg[1].productMarker,cfg[0].productMarker,'factory layout remains distinct');
});
test('C3 without an SD card preserves live audio and never claims SD ready from supported flags',()=>{
 const c=TARGETS[c3[0]];
 assert(c.features.includes('sdAudio'),'SD support is optional at runtime');
 const source=materialize(sketch,c.id);
 assert(source.includes('if (odysseySdDetectionState()==1) {'), 'ready requires actual mounted SD');
 assert(source.includes('odysseySdReady()'), 'no-card safety gate');
 assert(source.includes('streamingEnabled.store(false)'), 'BLE audio independent of SD presence');
});
test('S3 SuperMini remains a live-audio device, not an accidental SD recorder',()=>{
 const t=TARGETS['esp32s3-fh4r2-qspi-4m'];
 assert(!t.features.includes('sdAudio')&&!t.features.includes('camera'));
 assert(materialize(sketch,t.id).includes('odysseyDetectSdCard()'),'detection-only behavior');
});
test('Chakshu preserves common gestures and uses live SD readiness with visual-only voice',()=>{
 const t=TARGETS['xiao-esp32s3-sense-8m'],source=materialize(sketch,t.id);
 assert.equal(t.hardware.touch,1);
 assert.equal(t.hardware.led,5);
 assert(source.includes('setPinsPdmRx(42,41)'));
 assert(source.includes('ChakshuVoice::touchAudioToggle()'));
 assert(source.includes('if(remoteStandby){')&&source.includes('remoteStandby=false;'),'offline after standby');
 assert(source.includes('ChakshuStorage::ready.load()'),'live SD capability');
 assert(source.includes('ChakshuCamera::ready'),'live camera capability');
 assert(source.includes('ChakshuStorage::sdQuarantined.load()'),'timeout protection');
 assert(source.includes('command==PHOTO || command==DESCRIBE'),'visual-only intent');
 assert(!source.includes('if(command==AUDIO_ON)'),'no voice-triggered audio capture');
});
