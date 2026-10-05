'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');
function productionS3(){return fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8')}

test('final production S3 source retains core audio, touch, low-power and OTA contract',()=>{
  const s3=productionS3();
  assert.match(s3,/#define SYNAP_TOUCH_PIN 13/);
  assert.match(s3,/AUDIO_PROTOCOL_VERSION = 3/);
  assert.match(s3,/MIC_START_ATTEMPTS=3/);
  assert.match(s3,/TOUCH_DOUBLE_TAP_GAP_MS = 550/);
  assert.match(s3,/TOUCH_WAKE_HOLD_MS = 4000/);
  assert.match(s3,/CMD_RESTART = 0x05/);
  assert.match(s3,/publishPowerEvent\(POWER_STATE_DEEP_SLEEP\)/);
});

test('C3 clean-room recorder is write-only and owns a bounded SD session',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/clean-room C3 recorder ready; mount deferred to offline double tap/);
  assert.match(c3,/ODYSSEY_SD_SPI_HZ=400000u/);
  assert.match(c3,/odysseyCleanResyncBeforeMount/);
  assert.match(c3,/odysseyCleanIdleClocks\(20\)/);
  assert.match(c3,/uint8_t r1=odysseyCleanCmd0\(\)/);
  assert.match(c3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_SPI_HZ/);
  assert.match(c3,/SD\.open\(path,FILE_WRITE\)/);
  assert.match(c3,/odysseyCleanWavHeader\(header,pcmBytes\)/);
  assert.match(c3,/file\.seek\(0,SeekSet\)/);
  assert.match(c3,/clean PCM capture active/);
  assert.match(c3,/odysseyCaptureActive=true/);
  assert.match(c3,/clean WAV saved/);
  assert.match(c3,/delay\(20\);\s*odysseyCleanUnmount\(\)/);
  assert.match(c3,/bool available\(\) \{ return false; \}/);
  assert.doesNotMatch(c3,/readSelected\(/);
  assert.doesNotMatch(c3,/"@catalogue"/);
  assert.doesNotMatch(c3,/SYNAPJ01|SYNAPM01/);
  assert.doesNotMatch(c3,/Preallocate|preallocate|SEGMENT|segmentCrc/);
  assert.doesNotMatch(c3,/odysseyRecoverSdCard/);
});

test('clean recorder preserves the pinned BLE toolchain while SD is isolated',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
});
