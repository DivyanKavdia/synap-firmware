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

test('C3 production image uses native ESP-IDF SDSPI for write-only offline WAVs',()=>{
  const c3=materialize(productionS3(),'esp32c3-supermini-4m');
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
  assert.match(c3,/native C3 electrical-safe recorder ready; 400kHz\/8kPCM mount deferred to offline double tap/);
  assert.match(c3,/SDSPI_HOST_DEFAULT\(\)/);
  assert.match(c3,/spi_bus_initialize\(ODYSSEY_SD_HOST/);
  assert.match(c3,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT/);
  assert.match(c3,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT/);
  assert.match(c3,/host\.max_freq_khz=ODYSSEY_SD_SPI_KHZ/);
  assert.match(c3,/ODYSSEY_SD_SPI_KHZ=400u/);
  assert.match(c3,/ODYSSEY_SD_WAV_RATE=8000u/);
  assert.match(c3,/gpio_pullup_en/);
  assert.match(c3,/open\(path,O_CREAT\|O_EXCL\|O_WRONLY,0644\)/);
  assert.match(c3,/odysseyCleanWavHeader\(header,pcmBytes\)/);
  assert.match(c3,/lseek\(file,0,SEEK_SET\)/);
  assert.match(c3,/\[SD-IDF\] PCM capture active/);
  assert.match(c3,/odysseyCaptureActive=true/);
  assert.match(c3,/\[SD-IDF\] WAV saved/);
  assert.match(c3,/bool available\(\) \{ return false; \}/);
  assert.doesNotMatch(c3,/readSelected\(/);
  assert.doesNotMatch(c3,/"@catalogue"/);
  assert.doesNotMatch(c3,/SYNAPJ01|SYNAPM01/);
  assert.doesNotMatch(c3,/Preallocate|preallocate|SEGMENT|segmentCrc/);
  assert.doesNotMatch(c3,/odysseyRecoverSdCard/);
});

test('C3 source branch does not compile through Arduino SD.h or SPIClass',()=>{
  const source=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-clean-recording.cpp'),'utf8');
  const c3=source.slice(source.indexOf('#if CONFIG_IDF_TARGET_ESP32C3'),source.indexOf('#elif CONFIG_IDF_TARGET_ESP32S3'));
  assert.doesNotMatch(c3,/<SD\.h>|<SPI\.h>|SPIClass|SD\.begin|SD\.end/);
  assert.doesNotMatch(c3,/sdWriteSector|sdWriteSectors|SYNAP_C3_SD_WRITE_ACCEPTED|SYNAP_C3_SD_SINGLE_SECTOR_ONLY/);
});

test('release keeps pinned Arduino core but applies no Arduino SD source patch',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.doesNotMatch(workflow,/patch-arduino-sd\.cjs|patch-c3-sd-write\.cjs|SYNAP_ARDUINO_SD_SRC/);
});
