'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepareProduction}=require('../tools/prepare-production.cjs');
const root=path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');

test('release preparation copies the production source byte for byte and is idempotent',()=>{
  assert.equal(prepareProduction(source),source);
  assert.equal(prepareProduction(prepareProduction(source)),source);
  assert.throws(()=>prepareProduction(''),/primary S3/);
});

test('production battery, identity and OTA contracts remain in the reviewed source',()=>{
  assert.match(source,/SYNAP-FW:esp32s3-fh4r2-qspi-4m:1\.0\.0:/);
  assert.match(source,/900000u/,'OTA resume survives mobile background suspension');
  assert.match(source,/#if CONFIG_IDF_TARGET_ESP32S3\s*\n#define SYNAP_BATTERY_MONITOR_ENABLE 1\s*\n#else\s*\n#define SYNAP_BATTERY_MONITOR_ENABLE 0/);
  assert.match(source,/BATTERY_DIVIDER_TOP_OHMS = 1000000u/);
  assert.match(source,/BATTERY_DIVIDER_BOTTOM_OHMS = 470000u/);
  assert.match(source,/BATTERY_SAMPLE_MS = 15000u/);
  assert.match(source,/#define SYNAP_BATTERY_ADC_PIN 8/);
  assert.match(source,/analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_6db\)/);
  assert.match(source,/batteryAvailable=batteryValidSamples>=1/);
  assert.match(source,/case CMD_GET_STATUS:[\s\S]*?sampleBattery\(true\)/);
  assert.match(source,/void publishBatteryEvent\(bool force\)[\s\S]*?controlCharacteristic->notify\(\);[\s\S]*?vTaskDelay\(pdMS_TO_TICKS\(20\)\)[\s\S]*?updateStatusCharacteristic\(false\)/);
  assert.match(source,/batteryMillivolts=uint16_t\(cellMv>65535u\?65535u:cellMv\)/);
  assert.match(source,/SYNAP-%02X%02X%02X%02X%02X%02X/);
  assert.match(source,/CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE/);
});

test('standalone builds use the microphone and identify an unpublished local build',()=>{
  assert.match(source,/#ifndef USE_REAL_I2S_MIC\s*\n#define USE_REAL_I2S_MIC 1/);
  assert.match(source,/#define SYNAP_BUILD 0/);
  assert.match(source,/I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6/);
});

test('release publishes only the current candidate and retains both exact target sources',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const publisher=fs.readFileSync(path.join(root,'tools/publish.cjs'),'utf8');
  for(const target of ['esp32s3','esp32c3'])
    assert.match(workflow,new RegExp('bundle/source-sync/synap_'+target+'\\.ino'));
  assert.doesNotMatch(workflow,/git push origin HEAD:main/,'generated artifacts must not mutate reviewed source');
  assert.match(workflow,/id: publish_result/);
  assert.match(workflow,/steps\.publish_result\.outputs\.published == 'true'/);
  assert.match(publisher,/setPublished\(false\).*Superseded source commit/s);
  assert.match(publisher,/setPublished\(true\);\s*console\.log\(`Published/);
});
