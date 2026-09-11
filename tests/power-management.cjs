'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.join(__dirname,'..');

test('production audio powers microphone and CPU only when needed',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  assert.match(source,/std::atomic<bool> microphoneValidated\{false\}/);
  assert.match(source,/bool startMicrophone\(\)/);
  assert.match(source,/void stopMicrophone\(\)/);
  assert.match(source,/microphoneI2S\.end\(\)/,'I2S clocks must stop while idle');
  assert.match(source,/if \(!startMicrophone\(\)\) \{ stopStreaming\(ErrorCode::AUDIO_SOURCE_FAILED\)/,'START must power the microphone on demand');
  assert.match(source,/microphoneValidated=startMicrophone\(\);[\s\S]*stopMicrophone\(\)/,'boot should probe then power down the microphone');
  assert.match(source,/&& microphoneValidated/,'OTA rollback validation must not require I2S to remain running');
  assert.match(source,/IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240/,'S3 idle and active CPU profiles must be explicit');
  assert.match(source,/applyCpuPowerProfile\(streamingEnabled\.load\(\) \|\| otaBusy\(\)\)/,'recording and OTA must restore the active CPU profile');
  assert.match(source,/ulTaskNotifyTake\(pdTRUE, portMAX_DELAY\)/,'idle capture blocks until START');
  assert.match(source,/streamingEnabled.store\(true\);\s*if \(captureTaskHandle\) xTaskNotifyGive\(captureTaskHandle\)/,'START wakes capture immediately');
});
