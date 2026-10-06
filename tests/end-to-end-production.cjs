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

test('C3 production image restores the retained-mount 1481 SD lifecycle',()=>{\n  const c3=materialize(productionS3(),'esp32c3-supermini-4m');\n  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);\n  assert.match(c3,/static SPIClass odysseySdSpi\\(FSPI\\)/);\n  assert.match(c3,/ODYSSEY_SD_DATA_FREQ_HZ=400000u/);\n  assert.match(c3,/SD\\.begin\\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ/);\n  assert.match(c3,/odysseyLegacyMount\\("boot"\\)/);\n  assert.match(c3,/reuse a healthy retained mount/i);\n  assert.match(c3,/file\\.write\\(reinterpret_cast<const uint8_t\\*>\\(pcm\\),sizeof\\(pcm\\)\\)/);\n  assert.match(c3,/millis\\(\\)-checkpointAt\\)>=2000u/);\n  assert.match(c3,/file\\.flush\\(\\)/);\n  assert.match(c3,/bool available\\(\\) \\{ return false; \\}/);\n  assert.doesNotMatch(c3,/esp_vfs_fat_sdspi_mount|spi_bus_initialize|ODYSSEY_SD_WAV_RATE|gpio_pullup_en/);\n});\n\ntest('C3 healthy take does not unmount before the next offline recording',()=>{\n  const source=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-clean-recording.cpp'),'utf8');\n  const c3=source.slice(source.indexOf('#if CONFIG_IDF_TARGET_ESP32C3'),source.indexOf('#elif CONFIG_IDF_TARGET_ESP32S3'));\n  const record=c3.split('static void odysseyLegacyRecordTask(void*) {')[1].split('void odysseyInitializeSdCardBeforeBle()')[0];\n  assert.match(record,/if \\(failed\\) \\{[\\s\\S]*SD\\.end\\(\\)/);\n  assert.doesNotMatch(record,/else if \\(bytes\\)[\\s\\S]*SD\\.end\\(\\)/);\n});\n\ntest('release keeps pinned Arduino core but applies no Arduino SD source patch',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  const compileLines=workflow.split('\n').filter(line=>line.includes('arduino-cli compile'));
  assert.equal(compileLines.length,3);
  assert(compileLines.every(line=>line.includes('-DUSE_REAL_I2S_MIC=1')));
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.doesNotMatch(workflow,/patch-arduino-sd\\.cjs|patch-c3-sd-write\\.cjs|SYNAP_ARDUINO_SD_SRC/);
});
