'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:event}=require('../tools/patch-event-channel.cjs');
const {patch:battery}=require('../tools/patch-battery-v2.cjs');
const {patch:audio}=require('../tools/patch-audio-reliability.cjs');
const root=path.join(__dirname,'..');

test('production audio patch powers microphone and CPU only when needed',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const prepared=audio(battery(event(runtime(prepare(source)))));
  assert.match(prepared,/bool microphoneValidated = false/);
  assert.match(prepared,/bool startMicrophone\(\)/);
  assert.match(prepared,/void stopMicrophone\(\)/);
  assert.match(prepared,/microphoneI2S\.end\(\)/,'I2S clocks must stop while idle');
  assert.match(prepared,/if \(!startMicrophone\(\)\) \{ stopStreaming\(ErrorCode::AUDIO_SOURCE_FAILED\)/,'START must power the microphone on demand');
  assert.match(prepared,/microphoneValidated=startMicrophone\(\);[\s\S]*stopMicrophone\(\)/,'boot should probe then power down the microphone');
  assert.match(prepared,/&& microphoneValidated/,'OTA rollback validation must not require I2S to remain running');
  assert.match(prepared,/IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240/,'S3 idle and active CPU profiles must be explicit');
  assert.match(prepared,/applyCpuPowerProfile\(streamingEnabled\.load\(\) \|\| otaBusy\(\)\)/,'recording and OTA must restore the active CPU profile');
  assert.match(prepared,/vTaskDelay\(pdMS_TO_TICKS\(40\)\)/,'idle capture task must avoid 100 Hz wakeups');
});
