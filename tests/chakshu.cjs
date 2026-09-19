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


test('Chakshu starts one-model voice only after BLE-safe runtime readiness',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const setup=source.indexOf('void setup()');
  const loop=source.indexOf('void loop()');
  const ble=source.indexOf('\n  initializeBLE();',setup);
  const lazy=source.indexOf('millis()>=START_AFTER_MS',loop);
  assert(setup>=0 && loop>setup);
  assert(ble>setup && ble<loop);
  assert(lazy>loop);
  assert.match(source,/constexpr uint32_t START_AFTER_MS=8000/);
  assert.match(source,/ChakshuResources::runtimeReady\.load\(\)/);
  assert.match(source,/single-model v%u ready/);
  assert.match(source,/constexpr size_t INPUTS=64,H1=24,H2=12,OUTPUTS=6/);
  assert.match(source,/a9985b4e30f11bc855e4dbe818d6f2432272d0a994592d594f8b5dcf78e77bbb/);
  assert.doesNotMatch(source,/esp_afe_sr_iface|esp_mn_models|esp_wn_models|srmodel_load|WakeNet|MultiNet/);
});

test('single-model voice maps armed commands to SD-first actions',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  assert.match(source,/HEY_SNAP=1,TAKE_PHOTO=2,TAKE_VIDEO=3,RECORD_AUDIO=4,STOP=5/);
  assert.match(source,/constexpr uint32_t COMMAND_WINDOW_MS=4500/);
  assert.match(source,/if\(!queueLocal\(11\)\)result=1/);
  assert.match(source,/queueLocal\(5,uint32_t\(25\)<<8\)/);
  assert.match(source,/queueLocal\(10,600\)/);
  assert.match(source,/if\(offline\.load\(\)\)stopRequested\.store\(true\)/);
  assert.match(source,/const uint16_t value=command==VIDEO_START\?25:0/);
  assert.match(source,/ChakshuResources::Lease admission/);
});

