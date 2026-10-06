'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const source=()=>fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
const c3=()=>{const s=source();return s.slice(s.indexOf('#if CONFIG_IDF_TARGET_ESP32C3'),s.indexOf('#elif CONFIG_IDF_TARGET_ESP32S3'));};

test('C3 restores the production-1481 Arduino SD mount path at 400 kHz',()=>{
 const s=c3();
 assert.match(s,/#include <SPI\.h>/);
 assert.match(s,/#include <SD\.h>/);
 assert.match(s,/static SPIClass odysseySdSpi\(FSPI\)/);
 assert.match(s,/ODYSSEY_SD_DATA_FREQ_HZ=400000u/);
 assert.match(s,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,[\s\S]*?"\/odyssey-sd",1,false\)/);
 assert.doesNotMatch(s,/esp_vfs_fat_sdspi_mount|spi_bus_initialize|SDSPI_HOST_DEFAULT/);
 assert.doesNotMatch(s,/gpio_pullup_en|ODYSSEY_SD_WAV_RATE|8000u/);
});

test('boot mounts once before BLE and a healthy offline take reuses the retained mount',()=>{
 const s=c3();
 const boot=s.split('void odysseyInitializeSdCardBeforeBle() {')[1].split('void odysseyToggleRecording()')[0];
 const toggle=s.split('void odysseyToggleRecording() {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert.match(boot,/odysseyLegacyMount\("boot"\)/);
 assert.match(boot,/retained before BLE/);
 assert.match(toggle,/reuse a healthy retained mount/i);
 assert.match(toggle,/if \(odysseySdBootState\.load\(\)!=1 \|\| SD\.cardType\(\)==CARD_NONE\)/);
 assert.match(toggle,/odysseyLegacyMount\("touch"\)/);
 assert.doesNotMatch(toggle,/odysseyLegacyMount\("touch"\)[\s\S]*odysseyLegacyMount\("touch"\)/);
});

test('offline WAV writer matches the known-good 1481 frame and checkpoint shape',()=>{
 const s=c3();
 const record=s.split('static void odysseyLegacyRecordTask(void*) {')[1].split('void odysseyInitializeSdCardBeforeBle()')[0];
 assert.match(record,/int16_t pcm\[SAMPLES_PER_FRAME\]/);
 assert.match(record,/file\.write\(reinterpret_cast<const uint8_t\*>\(pcm\),sizeof\(pcm\)\)/);
 assert.match(record,/uint32_t\(millis\(\)-checkpointAt\)>=2000u/);
 assert.match(record,/file\.seek\(0\)[\s\S]*file\.write\(header,44\)!=44[\s\S]*file\.seek\(44\+bytes\)/);
 assert.match(record,/file\.flush\(\)/);
 assert.doesNotMatch(record,/fsync\(|\.part|ODYSSEY_SD_WRITE_CHUNK_BYTES|ODYSSEY_SD_WAV_RATE/);
});

test('successful recordings keep the card mounted; power transitions explicitly release it',()=>{
 const s=c3();
 const record=s.split('static void odysseyLegacyRecordTask(void*) {')[1].split('void odysseyInitializeSdCardBeforeBle()')[0];
 const power=s.split('bool odysseyPrepareSdForPowerTransition')[1].split('namespace OdysseyTransfer')[0];
 assert.match(record,/if \(failed\) \{[\s\S]*SD\.end\(\)/);
 assert.doesNotMatch(record,/else if \(bytes\)[\s\S]*SD\.end\(\)/);
 assert.match(power,/odysseyLegacyRelease\(true\)/);
});

test('purple means first full PCM frame was accepted and transfer stays disabled',()=>{
 const s=c3();
 const record=s.split('static void odysseyLegacyRecordTask(void*) {')[1].split('void odysseyInitializeSdCardBeforeBle()')[0];
 assert.match(record,/written!=sizeof\(pcm\)[\s\S]*if \(!captureConfirmed\)[\s\S]*odysseyCaptureActive=true/);
 assert.match(s,/namespace OdysseyTransfer \{[\s\S]*bool available\(\) \{ return false; \}/);
});
