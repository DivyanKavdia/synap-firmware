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

test('C3 production image uses guarded append-only multi-block offline recording',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(c3,/ODYSSEY_SD_DATA_FREQ_HZ=1000000u/);
  assert.match(c3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ/);
  assert.match(c3,/static void odysseyRecordTake\(\)/);
  assert.match(c3,/OdysseySdGuard storage/);
  assert.match(c3,/file=fopen\(fullPath,"wb"\)/);
  assert.doesNotMatch(c3,/esp_vfs_fat_create_contiguous_file|ODYSSEY_SD_RECORD_RESERVE_BYTES|ftruncate\(|odysseyFinalizeWav|fseek\(file,0/);
  assert.doesNotMatch(c3,/stat\(fullPath/);
  assert.doesNotMatch(c3,/checkpointAt|odysseyCheckpointWav/);
  assert.match(c3,/ODYSSEY_SD_MAX_OPEN_FILES=1/);
  assert.match(c3,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(c3,/setvbuf\(file,nullptr,_IONBF,0\)/);
  assert.match(c3,/fwrite\(batch,1,sizeof\(batch\),file\)/);
  assert.match(c3,/memset\(batch\+batchUsed,0,sizeof\(batch\)-batchUsed\)/);
  assert.match(c3,/fclose\(file\)/);
  assert.doesNotMatch(c3,/uint8_t sector\[512\]|fwrite\(sector/);
  assert.match(c3,/failureStage=41/);
  assert.match(c3,/failureStage=42/);
  assert.match(c3,/failureStage=43/);
  assert.doesNotMatch(c3,/failureStage=45|failureStage=46|failureStage=65/);
  assert.match(c3,/failureStage=47/);
  assert.match(c3,/case EIO: return 49/);
  assert.match(c3,/case ENODEV: return 50/);
  assert.match(c3,/case ENOSPC: return 52/);
  assert.match(c3,/case EIO: return 66/);
  assert.match(c3,/case ENOSPC: return 68/);
  assert.match(c3,/const uint8_t persistedStage=failureStage\?failureStage:40/);
  assert.match(c3,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(c3,/previousRecordStage>=44u/);
  assert.match(c3,/previousRecordStage!=48u/);
  assert.match(c3,/static void virtualWavHeader/);
  assert.match(c3,/patchVirtualWavHeader\(bytes,size,offset,total\)/);
  assert.doesNotMatch(c3,/odysseySustainedWriteProbe|\.synap-sustained-write\.tmp/);
  assert.doesNotMatch(c3,/odysseyLegacyRecordTask|file\.write\(reinterpret_cast<const uint8_t\*>\(pcm\)/);
  assert.doesNotMatch(c3,/esp_vfs_fat_sdspi_mount|ODYSSEY_SD_WAV_RATE/);
});

test('1631 C3 worker exposes media-v1 plus BLE-controlled direct Wi-Fi upload',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/xTaskCreate\(worker,"odyssey-sd",TRANSFER_STACK_BYTES/);
  assert.match(c3,/odysseySdConsumeRecoveryRequest\(\)/);
  assert.match(c3,/bool available\(\)\{return requests!=nullptr;\}/);
  assert.match(c3,/createCharacteristic\("4fa12354-0000-1000-8000-00805f9b34fb"/);
  assert.match(c3,/createCharacteristic\("4fa12355-0000-1000-8000-00805f9b34fb"/);
  assert.match(c3,/case 7:\s*error=catalogue\(total\)/);
  assert.match(c3,/case 4:\s*error=readSelected\(request\.path,request\.offset,total,bytes,size\)/);
  assert.match(c3,/case 17: error=removeFile\(request\.path\)/);
  assert.match(c3,/case 18:/);
  assert.match(c3,/case 23:/);
  assert.match(c3,/case 24:/);
  assert.match(c3,/request\.operation==25/);
  assert.match(c3,/case 26:/);
  assert.match(c3,/p\[16\]\|=2/,'C3 must advertise direct Wi-Fi upload');
  assert.doesNotMatch(c3,/case 19:/,'full-card format must remain disabled');
  assert.match(c3,/patchVirtualWavHeader\(bytes,size,offset,total\)/);
  assert.doesNotMatch(c3,/p\[16\]\|=4/,'C3 must not advertise destructive format');
});

test('release keeps Arduino 3.3.5 pinned and applies the CMD24 fix only before C3 compile',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.match(workflow,/node tools\/patch-arduino-sd\.cjs/);
  assert.match(workflow,/SYNAP_SD_CMD24_BUSY_FIX/);
  const patchAt=workflow.indexOf('node tools/patch-arduino-sd.cjs');
  const s3At=workflow.indexOf("--output-dir compiled-s3");
  const chakshuAt=workflow.indexOf("--output-dir compiled-chakshu");
  const c3At=workflow.indexOf("--output-dir compiled-c3");
  assert(s3At>=0 && chakshuAt>s3At && patchAt>chakshuAt && c3At>patchAt,
    'SD core patch must affect C3 only, after S3 and Chakshu are compiled');
});
