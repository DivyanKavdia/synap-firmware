'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:events}=require('../tools/patch-event-channel.cjs');
const {patch:battery}=require('../tools/patch-battery-v2.cjs');
const {patch:audio}=require('../tools/patch-audio-reliability.cjs');
const {patch:codec}=require('../tools/patch-audio-codec-v3.cjs');
const {patch:touch}=require('../tools/patch-touch-reliability.cjs');
const {patch:harden}=require('../tools/patch-production-hardening.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
function productionS3(){let s=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');for(const fn of [prepare,runtime,events,battery,audio,codec,touch,harden])s=fn(s);return s}

test('final production S3 source matches the shipped audio, touch, power and OTA contract',()=>{
  const s3=productionS3();
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/,'production touch pin must live in the source artifact, not only compiler flags');
  assert.match(s3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(s3,/ADPCM_BYTES_PER_FRAME == 404/);
  assert.match(s3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(s3,/MIN_REQUIRED_MTU = 32/);assert.match(s3,/MAX_AUDIO_PAYLOAD_BYTES = 500/);
  assert.match(s3,/MIC_START_ATTEMPTS=3[\s\S]*?attempt<=MIC_START_ATTEMPTS/,'I2S initialization must retry after OTA or wake reinitialization');
  assert.match(s3,/bool microphoneRecoveryUsed=false[\s\S]*?empty I2S reads; restarting capture driver[\s\S]*?stopMicrophone\(\)[\s\S]*?startMicrophone\(\)/,'live capture must self-heal one stalled I2S driver before failing');
  assert.match(s3,/uint8_t emptyReads=0[\s\S]*?\+\+emptyReads < 3/,'transient I2S timeouts must not kill a take immediately');
  assert.match(s3,/uint16_t liveMtu=bleServer->getPeerMTU[\s\S]*?liveCapacity < uint16_t\(AUDIO_HEADER_BYTES\+audioPayloadBytes\.load\(\)\)[\s\S]*?peerMtu=liveMtu/,'compatible asynchronous MTU changes must stay live');
  assert.doesNotMatch(s3,/getPeerMTU\(bleServer->getConnId\(\)\) != peerMtu[\s\S]*?stopStreaming\(ErrorCode::TRANSPORT_CHANGED\)/);
  assert.match(s3,/TOUCH_START_HOLD_MS = 2000/);assert.match(s3,/TOUCH_SLEEP_HOLD_MS = 5000/);assert.match(s3,/TOUCH_TAP_MAX_MS = 450/);
  assert.match(s3,/double tap while recording -> STOP/);assert.match(s3,/wake detected; hold for 5 seconds to stay awake/);
  const poll=s3.slice(s3.indexOf('void pollTouchControl() {'),s3.indexOf('\n}\n',s3.indexOf('void pollTouchControl() {'))+3);
  assert.doesNotMatch(poll,/publishRememberEvent|TOUCH_STOP_MIN_MS|TOUCH_STOP_MAX_MS/,'legacy touch gestures must be absent');
  assert.match(s3,/enterDeepSleep[\s\S]*?digitalRead\(TOUCH_INPUT_PIN\)==TOUCH_ACTIVE_LEVEL\) return/,'sleep must wait for touch release');
  assert.match(s3,/batteryCritical\(\) && otaBusy\(\)[\s\S]*?otaSession\.fail\(Synap::BUSY\)/,'critical battery must stop an in-progress OTA');
  assert.match(s3,/1\.32 V ADC for a 4\.13 V cell on the 1M\/470k divider/);
  assert.match(s3,/xTaskCreatePinnedToCore\(transmitterTask, "transmit", 8192/);
});

test('C3 materialization preserves the same recording protocol with target-safe hardware differences',()=>{
  const s3=productionS3(),c3=materialize(s3,'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);assert.match(c3,/#define SYNAP_BATTERY_ADC_PIN 1/);assert.doesNotMatch(c3,/GPIO8/);
  assert.match(c3,/AUDIO_PROTOCOL_VERSION = 3/);assert.match(c3,/MIN_CHUNKS_PER_FRAME = 1/);assert.match(c3,/MIN_REQUIRED_MTU = 32/);
  assert.match(c3,/MIC_START_ATTEMPTS=3/);assert.match(c3,/microphoneRecoveryUsed=false/);
  assert.match(c3,/TOUCH_START_HOLD_MS = 2000/);assert.match(c3,/TOUCH_SLEEP_HOLD_MS = 5000/);
  assert.match(c3,/xTaskCreate\(transmitterTask, "transmit", 8192/);assert.doesNotMatch(c3,/xTaskCreatePinnedToCore/);
  assert.match(c3,/SYNAP_BATTERY_MONITOR_ENABLE 0/,'C3 battery monitor stays disabled until its divider is physically audited');
  assert.match(c3,/esp_deep_sleep_enable_gpio_wakeup/);assert.doesNotMatch(c3,/esp_sleep_enable_ext1_wakeup/);
});

test('release workflow compiles auditable sources and real microphones for both production targets',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  assert.match(workflow,/patch-production-hardening\.cjs/);
  assert.doesNotMatch(workflow,/-DSYNAP_TOUCH_PIN=13/,'touch pin must not be a hidden compiler-only override');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,2);assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')),'S3 and C3 release binaries must use real I2S capture');
});
