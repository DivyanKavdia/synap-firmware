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

test('C3 clean-room recorder is write-only and excludes the legacy SD stack',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/Odyssey C3 clean-room offline SD recorder/);
  assert.match(c3,/ODYSSEY_SD_DATA_FREQ_HZ=1000000u/);
  assert.match(c3,/ODYSSEY_SD_WRITE_CHUNK_BYTES=512u/);
  assert.match(c3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ/);
  assert.match(c3,/SD\.open\(temp,FILE_WRITE\)/);
  assert.match(c3,/SD\.rename\(temp,wav\)/);
  assert.match(c3,/bool available\(\)\{return false;\}/);
  assert.match(c3,/double tap -> fresh SD audio START/);
  assert.match(c3,/fresh PCM capture active/);
  assert.doesNotMatch(c3,/odysseyRecordTake|odysseyRecoverSdCard|odysseySdRecoverCardProtocolLocked/);
  assert.doesNotMatch(c3,/readSelected\(|"@catalogue"|SYNAPJ01|SYNAPM01/);
  assert.doesNotMatch(c3,/esp_vfs_fat_create_contiguous_file|ODYSSEY_INLINE_JOURNAL/);
});

test('release keeps core 3.3.5 and applies the SD initializer backport only before C3',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.match(workflow,/tools\/patch-arduino-sd\.cjs/);
  const s3=workflow.indexOf('--output-dir compiled-s3');
  const chakshu=workflow.indexOf('--output-dir compiled-chakshu');
  const patch=workflow.indexOf('node tools/patch-arduino-sd.cjs');
  const c3=workflow.indexOf('--output-dir compiled-c3');
  assert.ok(s3>0&&chakshu>s3&&patch>chakshu&&c3>patch,
    'SD backport must affect only the C3 compile');
});
