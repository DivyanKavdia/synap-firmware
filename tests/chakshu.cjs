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


test('Chakshu TinyML starts only after BLE and uses no ESP-SR runtime',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const ble=source.indexOf('initializeBLE();');
  const schedule=source.indexOf('  ChakshuVoice::scheduleInitialize();',ble);
  assert(ble>=0 && schedule>ble);
  assert.match(source,/TinyML remains optional and starts only after the proven BLE\/OTA\/media boot path is healthy/);
  assert.match(source,/WINDOW_SAMPLES=24000/);
  assert.match(source,/LEARNED_WEIGHT_BYTES=sizeof\(C1_WEIGHT\)\+sizeof\(C2_WEIGHT\)\+sizeof\(FC_WEIGHT\)/);
  assert.match(source,/xTaskCreatePinnedToCore\(workerTask,"tiny-voice",12288/);
  assert.match(source,/xTaskCreatePinnedToCore\(idleTask,"tiny-listen",4096/);
  assert.doesNotMatch(source,/esp_afe_sr|esp_mn_|model_path|SYNAP_EMBEDDED_SR_MODEL_START/);
  assert.match(source,/4fa12356-0000-1000-8000-00805f9b34fb/);
  assert.match(source,/4fa12357-0000-1000-8000-00805f9b34fb/);
  assert.match(source,/4fa12358-0000-1000-8000-00805f9b34fb/);
});

test('TinyML v2 keeps actions deliberately small and SD-first',()=>{
  const voice=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice.cpp'),'utf8');
  const model=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/tiny-voice-model.h'),'utf8');
  const contract=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice-contract.cpp'),'utf8');
  assert.match(model,/CLASSES=6/);
  assert.match(model,/NOISE=0, UNKNOWN=1, HEY_SNAP=2, PHOTO=3, VIDEO=4, STOP=5/);
  assert.match(model,/C1_WEIGHT\[720\]/);
  assert.match(model,/C2_WEIGHT\[1200\]/);
  assert.match(model,/FC_WEIGHT\[240\]/);
  assert.match(model,/Real-device runtime-window fit: Hey Snap 97\.8%, Photo 100%, Video 96\.2%, Stop 94\.4%/);
  assert.match(voice,/streakCount<2/);
  assert.match(voice,/command==WAKE\?0\.72f:0\.76f/);
  assert.match(voice,/result\.margin<0\.10f/);
  assert.match(voice,/queueLocal\(11\)/);
  assert.match(voice,/queueLocal\(5,uint32_t\(10u\)<<8\)/);
  assert.doesNotMatch(voice,/speechHoldUntil-now\)>0&&!mediaBusy\(\)/);
  assert.doesNotMatch(voice,/queueLocal\(10/);
  assert.match(contract,/confidence<0\.70f/);
  assert.match(contract,/confidence<0\.74f/);
});





