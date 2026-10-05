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

test('C3 clean-room control build contains no compiled SD storage implementation',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/clean-room control: Odyssey C3 SD disabled/);
  assert.match(c3,/bool available\(\) \{ return false; \}/);
  assert.match(c3,/double tap -> SD disabled in control build/);
  assert.doesNotMatch(c3,/odysseyRecordTake/);
  assert.doesNotMatch(c3,/odysseyRecoverSdCard/);
  assert.doesNotMatch(c3,/odysseySdRecoverCardProtocolLocked/);
  assert.doesNotMatch(c3,/readSelected\(/);
  assert.doesNotMatch(c3,/"@catalogue"/);
  assert.doesNotMatch(c3,/SYNAPJ01|SYNAPM01/);
});

test('release workflow compiles the control build with the unchanged pinned toolchain',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
});
