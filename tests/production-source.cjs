'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch}=require('../tools/patch-runtime-fixes.cjs');
const root=path.join(__dirname,'..');

test('production preparation is reproducible and retains mobile OTA safely',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const prepared=patch(prepare(source));
  assert.match(prepared,/SYNAP-FW:esp32s3-fh4r2-qspi-4m:0\.0\.1:/);
  assert.match(prepared,/900000u/,'OTA resume must survive mobile background suspension');
  assert.match(prepared,/#if CONFIG_IDF_TARGET_ESP32S3\s*\n#define SYNAP_BATTERY_MONITOR_ENABLE 1\s*\n#else\s*\n#define SYNAP_BATTERY_MONITOR_ENABLE 0/,'battery monitor must be enabled only on audited ESP32-S3 hardware');
  assert.match(prepared,/BATTERY_DIVIDER_TOP_OHMS = 1000000u/,'battery divider top resistor must be 1 MOhm');
  assert.match(prepared,/BATTERY_DIVIDER_BOTTOM_OHMS = 330000u/,'battery divider bottom resistor must match measured production hardware');
  assert.match(prepared,/BATTERY_SAMPLE_MS = 15000u/,'battery telemetry should refresh every 15 seconds');
  assert.match(prepared,/SYNAP_BATTERY_ADC_PIN\s*\n#define SYNAP_BATTERY_ADC_PIN 8/,'S3 battery sense pin must remain GPIO8');
  assert.match(prepared,/about 1\.04 V from a 4\.2 V cell through the 1M\/330k divider/,'ADC comment must reflect calibrated divider range');
  assert.match(prepared,/analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_6db\)/,'GPIO8 ADC must use calibrated attenuation');
  assert.match(prepared,/batteryAvailable=batteryValidSamples>=1/,'one averaged valid sample should make battery telemetry available');
  assert.match(prepared,/case CMD_GET_STATUS:[\s\S]*?sampleBattery\(true\)/,'fresh battery telemetry must be sent after the PWA subscribes and requests status');
  assert.match(prepared,/void publishBatteryEvent\(bool force\)[\s\S]*?controlCharacteristic->notify\(\);[\s\S]*?vTaskDelay\(pdMS_TO_TICKS\(20\)\)[\s\S]*?updateStatusCharacteristic\(false\)/,'battery notification must be allowed to leave before restoring control status');
  assert.match(prepared,/streamStartedAt=millis\(\)[\s\S]*?setDeviceState\(DeviceState::STREAMING/,'stream start time must be captured on the pendant');
  assert.match(prepared,/value\[3\]\|=0x04[\s\S]*?put32le\(value\+8,eventTime\)/,'memory packet must mark and carry stream-relative milliseconds');
  assert.match(prepared,/void publishRememberEvent\(\)[\s\S]*?controlCharacteristic->notify\(\);[\s\S]*?vTaskDelay\(pdMS_TO_TICKS\(20\)\)[\s\S]*?updateStatusCharacteristic\(false\)[\s\S]*?memoryAckUntil/,'memory marker must be allowed to leave before restoring control status');
  assert.match(prepared,/batteryMillivolts=uint16_t\(cellMv>65535u\?65535u:cellMv\)/,'invalid ADC readings should still expose measured voltage for diagnostics');
  assert.match(prepared,/\[BATTERY\] gpio=/,'battery sampling diagnostics must remain available over serial');
  assert.match(prepared,/vTaskDelay\(pdMS_TO_TICKS\(60\)\)/,'STOP must quiesce audio before acknowledgement');
  assert.match(prepared,/SYNAP-%02X%02X%02X%02X%02X%02X/,'public hardware ID must come from eFuse MAC');
  assert.match(prepared,/TOUCH_SLEEP_HOLD_MS/);
  assert.match(prepared,/publishRememberEvent/);
  assert.match(prepared,/CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE/);
});

test('audio source mode is explicit until production microphone hardware is confirmed',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  assert.match(source,/#ifndef USE_REAL_I2S_MIC\s*\n#define USE_REAL_I2S_MIC 0/);
  assert.match(source,/I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6/);
});

test('generated C3 source sync cannot create a second release build',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  assert.match(workflow,/bundle\/source-sync\/synap_esp32s3\.ino/,'verified artifact should retain the exact prepared S3 source');
  assert.match(workflow,/Sync explicit ESP32-C3 firmware source \[skip ci\]/,'generated C3 sync commit must not trigger another firmware workflow');
});