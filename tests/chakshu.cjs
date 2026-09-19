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

test('one MultiNet model classifies Hey Snap and commands behind a five-second firmware gate',()=>{
  const voice=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice.cpp'),'utf8');
  const contract=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice-contract.cpp'),'utf8');
  assert.match(voice,/addPhrase\(WAKE,"hd SNaP"\)/);
  assert.match(voice,/addPhrase\(PHOTO,"TdK c SNaP"\)/);
  assert.match(voice,/addPhrase\(VIDEO_START,"RcKeRD c VgDmb"\)/);
  assert.match(voice,/addPhrase\(AUDIO_ON,"RcKeRD eDmb"\)/);
  assert.match(voice,/esp_srmodel_filter\(models,"mn5q8","en"\)/);
  assert.match(voice,/mn->create\(name,5000\)/);
  assert.match(voice,/config->wakenet_init=false/);
  assert.doesNotMatch(voice,/esp_wn_models|WAKENET_DETECTED|wakeModels/);
  assert.match(contract,/uint32_t\(now-armedAt\)<=5000u/);
  assert.match(contract,/confidence<0\.55f/);
  assert.match(contract,/confidence<0\.72f/);
  assert.match(voice,/mn->set_det_threshold\(mnData,0\.45f\)/);
  assert.match(voice,/4fa12358-0000-1000-8000-00805f9b34fb/);
  assert.match(voice,/audioMeanAbs/);
  assert.match(voice,/candidateConfidence/);
  assert.match(voice,/queueLocal\(5,uint32_t\(25u\)<<8\)/);
  assert.match(voice,/Voice always performs local SD-first actions/);
  assert.match(voice,/while\(millis\(\)<8000u \|\| otaBusy\(\) \|\| mediaBusy\(\)\)/);
  assert.match(voice,/ChakshuResources::Lease admission;/);
});


