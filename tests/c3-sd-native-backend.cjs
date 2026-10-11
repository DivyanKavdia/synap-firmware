'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
const compiled=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('Odyssey C3 uses independent ESP-IDF SPI2 SDSPI FatFs, S3 keeps original SD detection',()=>{
  assert.match(detect,/#if CONFIG_IDF_TARGET_ESP32C3\s*#include <cerrno>[\s\S]*?#include "esp_vfs_fat.h"[\s\S]*?#include "driver\/sdspi_host.h"/);
  assert.match(detect,/#else\s*#include <SPI.h>\s*#include <SD.h>\s*#endif/);
  assert.match(detect,/static constexpr spi_host_device_t ODYSSEY_SD_HOST=SPI2_HOST;/);
  assert.match(detect,/spi_bus_initialize\(ODYSSEY_SD_HOST,&bus,SDSPI_DEFAULT_DMA\)/);
  assert.match(detect,/sdmmc_host_t host=SDSPI_HOST_DEFAULT\(\)/);
  assert.match(detect,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT,/);
  assert.match(detect,/esp_vfs_fat_sdcard_unmount\(/);
  assert.match(detect,/spi_bus_free\(ODYSSEY_SD_HOST\)/);
  assert.match(detect,/host\.max_freq_khz=ODYSSEY_SD_DATA_FREQ_HZ\/1000u/);
  assert.match(detect,/device\.gpio_cs=static_cast<gpio_num_t>\(ODYSSEY_SD_CS\)/);
  assert.match(detect,/#else\s*\/\/ Odyssey S3 remains detection-only[\s\S]*SD\.begin\(/);
  assert.match(compiled,/esp_vfs_fat_sdspi_mount\(/);
});

test('C3 boots without ADC gate, read-only FAT check and never formats automatically',()=>{
  assert.match(detect,/mount\.format_if_mount_failed=false/);
  assert.doesNotMatch(detect,/esp_vfs_fat_sdcard_format\(/);
  const validation=detect.split('static bool odysseySdValidateVfsLocked(')[1]
    .split('static uint8_t odysseySdMountReasonCode')[0];
  assert.doesNotMatch(validation,/fopen\(|fwrite\(|unlink\(|mkdir\(|fflush\(|remove\(/);
  assert.match(validation,/directoryErrno==ENOENT/);
  assert.match(boot,/restoreOdysseySdBatteryDividerProfile\(\);\s*sampleBattery\(true\);\s*odysseyInitializeSdCardBeforeBle\(\)/);
  assert.doesNotMatch(detect,/odysseySdPowerSafe\(/);
});

test('Native C3 preserves append-only WAV, durable checkpoints, safe STOP and read protocol',()=>{
  assert.match(rec,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u/);
  assert.match(rec,/fsync\(fd\)/);
  assert.match(rec,/fflush\(file\)/);
  assert.match(rec,/odysseyStopRequested\.load\(\)/);
  assert.match(rec,/fwrite\(/);
  assert.match(rec,/fclose\(file\)/);
  assert.match(rec,/OdysseySdGuard guard/);
  assert.match(transfer,/odysseySdPath\(/);
  assert.match(transfer,/fopen\(full,"rb"\)/);
  assert.match(detect,/sdmmc_get_status\(odysseySdNativeCard\)/);
  assert.match(detect,/if \(odysseyRecording\.load\(\)\) \{/);
});
