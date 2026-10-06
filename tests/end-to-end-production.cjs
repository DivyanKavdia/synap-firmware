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

test('C3 production image restores the build-1631 guarded VFS recorder',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(c3,/ODYSSEY_SD_DATA_FREQ_HZ=1000000u/);
  assert.match(c3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ/);
  assert.match(c3,/static void odysseyRecordTake\(\)/);
  assert.match(c3,/OdysseySdGuard storage/);
  assert.match(c3,/esp_vfs_fat_create_contiguous_file/);
  assert.match(c3,/ODYSSEY_SD_RECORD_RESERVE_BYTES=32u\*1024u\*1024u/);
  assert.match(c3,/file=fopen\(fullPath,"r\+b"\)/);
  assert.doesNotMatch(c3,/stat\(fullPath/);
  assert.doesNotMatch(c3,/checkpointAt|odysseyCheckpointWav/);
  assert.match(c3,/odysseyFinalizeWav/);
  assert.match(c3,/ODYSSEY_SD_MAX_OPEN_FILES=1/);
  assert.match(c3,/alignas\(4\) uint8_t sector\[512\]/);
  assert.match(c3,/setvbuf\(file,nullptr,_IONBF,0\)/);
  assert.match(c3,/fwrite\(sector,1,sizeof\(sector\),file\)/);
  assert.match(c3,/odysseySdRequestRecovery\(\)/);
  assert.match(c3,/failureStage=41/);
  assert.match(c3,/failureStage=42/);
  assert.match(c3,/failureStage=43/);
  assert.doesNotMatch(c3,/failureStage=45/);
  assert.match(c3,/failureStage=46/);
  assert.match(c3,/failureStage=47/);
  assert.match(c3,/case EIO: return 49/);
  assert.match(c3,/case ENODEV: return 50/);
  assert.match(c3,/case ENOSPC: return 52/);
  assert.match(c3,/case EIO: return 55/);
  assert.match(c3,/case ENODEV: return 56/);
  assert.match(c3,/case ENOSPC: return 57/);
  assert.match(c3,/case EIO: return 60/);
  assert.match(c3,/case ENOSPC: return 62/);
  assert.match(c3,/failureStage=65/);
  assert.match(c3,/const uint8_t persistedStage=failureStage\?failureStage:40/);
  assert.match(c3,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.doesNotMatch(c3,/odysseySustainedWriteProbe|\\.synap-sustained-write\\.tmp/);
  assert.doesNotMatch(c3,/odysseyLegacyRecordTask|file\.write\(reinterpret_cast<const uint8_t\*>\(pcm\)/);
  assert.doesNotMatch(c3,/esp_vfs_fat_sdspi_mount|ODYSSEY_SD_WAV_RATE/);
});

test('1631 recovery worker remains active but BLE media access is disabled',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/xTaskCreate\(worker,"odyssey-sd",TRANSFER_STACK_BYTES/);
  assert.match(c3,/odysseySdConsumeRecoveryRequest\(\)/);
  assert.match(c3,/bool available\(\)\{return false;\}/);
  assert.match(c3,/void ble\(BLEService\*\) \{[\s\S]*Intentionally disabled/);
});

test('release keeps the exact build-1631 Arduino core and no SD library patch',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.doesNotMatch(workflow,/patch-arduino-sd\.cjs|patch-c3-sd-write\.cjs|SYNAP_ARDUINO_SD_SRC/);
});
