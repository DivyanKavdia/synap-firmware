'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

test('native-IDF recorder saves exact PCM and preserves partials on write/sync/close failure',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const header=source.slice(
   source.indexOf('static void odysseyCleanWavHeader'),
   source.indexOf('static void odysseyNativeResetPins')
 );
 const recorder=source.slice(
   source.indexOf('static bool odysseyCleanWriteAll'),
   source.indexOf('namespace OdysseyTransfer')
 );
 const body=fs.readFileSync('tests/c3-clean-recording.cpp','utf8')
   .replace('// INSERT PRODUCTION',(header+recorder).replaceAll('/odyssey-sd/synap/',''));
 nativeTest(body,['-DUSE_REAL_I2S_MIC=1']);
});

test('C3 storage backend is native ESP-IDF SDSPI and does not use Arduino SD.h',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const c3=source.slice(
   source.indexOf('#if CONFIG_IDF_TARGET_ESP32C3'),
   source.indexOf('#elif CONFIG_IDF_TARGET_ESP32S3')
 );
 assert.match(c3,/driver\/spi_master\.h/);
 assert.match(c3,/driver\/sdspi_host\.h/);
 assert.match(c3,/esp_vfs_fat\.h/);
 assert.match(c3,/SDSPI_HOST_DEFAULT\(\)/);
 assert.match(c3,/spi_bus_initialize\(ODYSSEY_SD_HOST/);
 assert.match(c3,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT/);
 assert.match(c3,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT/);
 assert.match(c3,/spi_bus_free\(ODYSSEY_SD_HOST\)/);
 assert.match(c3,/host\.max_freq_khz=ODYSSEY_SD_SPI_KHZ/);
 assert.match(c3,/ODYSSEY_SD_SPI_KHZ=400u/);
 assert.match(c3,/ODYSSEY_SD_WAV_RATE=8000u/);
 assert.match(c3,/gpio_pullup_en\(static_cast<gpio_num_t>\(ODYSSEY_SD_CS\)\)/);
 assert.doesNotMatch(c3,/<SD\.h>|<SPI\.h>|SPIClass|SD\.begin|SD\.end/);
 assert.doesNotMatch(c3,/sdWriteSector|sdWriteSectors|CMD24|CMD25|odysseyCleanCmd0|odysseyCleanResync/);
});

test('native backend never formats implicitly and keeps one task as card owner',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const c3=source.slice(
   source.indexOf('#if CONFIG_IDF_TARGET_ESP32C3'),
   source.indexOf('#elif CONFIG_IDF_TARGET_ESP32S3')
 );
 assert.match(c3,/mount\.format_if_mount_failed=false/);
 assert.match(c3,/mount\.max_files=2/);
 assert.match(c3,/xTaskCreate\(odysseyCleanRecordTask,"sd-idf-audio"/);
 assert.match(c3,/mount deferred to offline double tap/);
 assert.match(c3,/bool available\(\) \{ return false; \}/);
});

test('native post-purple failures remain distinguishable after unmount',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 assert.match(source,/failureStage=4/);
 assert.match(source,/failureStage=5/);
 assert.match(source,/failureStage=6/);
 assert.match(source,/odysseySdBootState=2;odysseySdProbeStage=failureStage\?failureStage:4/);
 assert.match(source,/esp_err_to_name\(odysseyNativeLastError\)/);
});
