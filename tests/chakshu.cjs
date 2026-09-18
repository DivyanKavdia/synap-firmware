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


test('WakeNet gates the MultiNet command window and wake feedback stays SD-safe',()=>{
  const voice=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/voice.cpp'),'utf8');
  assert.match(voice,/wakeModels\?esp_srmodel_filter\(wakeModels,ESP_WN_PREFIX,"hiesp"\):nullptr/);
  assert.match(voice,/esp_srmodel_filter\(models,"mn5q8","en"\)/);
  assert.match(voice,/mn->create\(name,8000\)/);
  assert.match(voice,/config->wakenet_init=true/);
  assert.match(voice,/config->wakenet_mode=DET_MODE_95/);
  assert.match(voice,/result->wakeup_state==WAKENET_DETECTED/);
  assert.match(voice,/gate\.accept\(WAKE,1\.0f,now\);commandWindow=true/);
  assert.match(voice,/if\(!commandWindow\|\|!result->data\)continue;/);
  assert.match(voice,/const auto detected=mn->detect\(mnData,result->data\)/);
  assert.match(voice,/afe->enable_wakenet\(afeData\)/);
  assert.doesNotMatch(voice,/addPhrase\(WAKE/);
  const wake=voice.indexOf('result->wakeup_state==WAKENET_DETECTED');
  const gate=voice.indexOf('if(!commandWindow||!result->data)continue;',wake);
  const multi=voice.indexOf('mn->detect(mnData,result->data)',gate);
  assert(wake>=0 && gate>wake && multi>gate);
  assert.match(voice,/constexpr uint8_t WAKE_LED_PIN=21;/);
  assert.match(voice,/ChakshuResources::Lease admission;/);
  assert.match(voice,/if\(!admission\)return false; \/\/ Never toggle shared SD CS during recording\/transfer\./);
  const acquire=voice.indexOf('ChakshuResources::Lease admission;');
  const ledOn=voice.indexOf('digitalWrite(WAKE_LED_PIN,LOW);',acquire);
  const ledOff=voice.indexOf('digitalWrite(WAKE_LED_PIN,HIGH);',ledOn);
  assert(acquire>=0 && ledOn>acquire && ledOff>ledOn);
  assert.match(voice,/lastCommand=WAKE;lastResult=online\?2:0/);
  assert.match(voice,/\[VOICE\] Hi ESP detected/);
  assert.match(voice,/events->notify\(\)/);
});
