'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {getTarget}=require('../tools/targets.cjs');
const fs=require('node:fs'),path=require('node:path');
test('canonical modules reproduce the distributable source byte for byte',()=>{
  assert.equal(assemble(),fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8'));
});
test('Chakshu uses onboard PDM and does not configure absent hardware',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  assert.match(source,/#define SYNAP_MODULE_ID 3/);
  assert.match(source,/#define DEVICE_NAME "synap-Chakshu"/);
  assert.match(source,/setPinsPdmRx\(42,41\)/);
  assert.match(source,/I2S_MODE_PDM_RX, SAMPLE_RATE,\s+I2S_DATA_BIT_WIDTH_16BIT/);
  assert.match(source,/static int16_t raw\[SAMPLES_PER_FRAME\]/);
  assert.match(source,/const int32_t sample=raw\[i\];/);
  assert.doesNotMatch(source,/statusLed\.|digitalRead\(TOUCH_INPUT_PIN|pinMode\(TOUCH_INPUT_PIN|analogSetPinAttenuation|esp_deep_sleep_start\(/);
  assert.match(source,/SPI.begin\(7,8,9,21\)/);
  assert.match(source,/config.pin_d7=48/);
  assert.match(source,/SYNAP-CHAKSHU-OTA-ID-V3/);
  assert.doesNotMatch(source,/SYNAP-ESP32S3-OTA-ID-V3/);
  assert.match(source,/if \(!mediaBusy\(\) && !ChakshuVoice::active\(\)\) stopMicrophone\(\)/);
});
test('Chakshu keeps separate release paths, OTA marker and the deployed default 8MB slots',()=>{
  const target=getTarget('xiao-esp32s3-sense-8m');
  assert.equal(target.family,'esp32s3');
  assert.equal(target.slotSize,0x330000);
  assert.equal(target.psramBytes,8388608);
  assert.equal(target.partition,'default_8MB');
  assert.equal(target.assetStem,'chakshu');
  assert.notEqual(target.productMarker,getTarget('esp32s3-fh4r2-qspi-4m').productMarker);
});


test('Chakshu advertises BLE before scheduling local voice model initialization',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const ble=source.indexOf('initializeBLE();');
  const schedule=source.indexOf('ChakshuVoice::scheduleInitialize();');
  const direct=source.indexOf('ChakshuVoice::initialize();');
  assert(ble>=0 && schedule>ble);
  assert.equal(direct,-1);
  assert.match(source,/Voice remains deferred: BLE\/OTA\/media must become available before model loading/);
});

test('WakeNet Hi ESP gates the MultiNet command window while BLE remains boot-first',()=>{
  const voice=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice.cpp'),'utf8');
  const contract=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice-contract.cpp'),'utf8');
  assert.match(voice,/#include <esp_wn_models\.h>/);
  assert.match(voice,/esp_srmodel_filter\(wakeModels,ESP_WN_PREFIX,"hiesp"\)/);
  assert.match(voice,/config->wakenet_init=true/);
  assert.match(voice,/WAKENET_DETECTED/);
  assert.match(voice,/afe->disable_wakenet\(afeData\)/);
  assert.match(voice,/afe->enable_wakenet\(afeData\)/);
  assert.doesNotMatch(voice,/addPhrase\(WAKE,/);
  assert.match(voice,/addPhrase\(PHOTO,"TdK c SNaP"\)/);
  assert.match(voice,/addPhrase\(VIDEO_START,"RcKeRD c VgDmb"\)/);
  assert.match(voice,/esp_srmodel_filter\(models,"mn5q8","en"\)/);
  assert.match(voice,/mn->create\(name,5000\)/);
  assert.match(contract,/uint32_t\(now-armedAt\)<=5000u/);
  assert.match(voice,/queueLocal\(5,uint32_t\(25u\)<<8\)/);
  assert.match(voice,/while\(millis\(\)<8000u \|\| otaBusy\(\) \|\| mediaBusy\(\)\)/);
});


