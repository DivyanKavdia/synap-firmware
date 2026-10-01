'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {getTarget}=require('../tools/targets.cjs');
const {renderProfile}=require('../tools/device-profile.cjs');
const root=path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-detect.cpp'),'utf8');
const recording=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-recording.cpp'),'utf8');
const transfer=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-transfer.cpp'),'utf8');

test('C3 SD owner uses ESP-IDF SDSPI/FAT on the fixed Odyssey pins',()=>{
  const target=getTarget('esp32c3-supermini-4m');
  assert.deepEqual(target.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  assert.match(source,/esp_vfs_fat_sdspi_mount/);
  assert.match(source,/sdmmc_host_t host=SDSPI_HOST_DEFAULT\(\)/);
  assert.match(source,/spi_bus_initialize\(static_cast<spi_host_device_t>\(host\.slot\),&bus,SDSPI_DEFAULT_DMA\)/);
  assert.match(source,/slot\.gpio_cs=static_cast<gpio_num_t>\(ODYSSEY_SD_CS\)/);
  assert.match(source,/bus\.sclk_io_num=ODYSSEY_SD_SCK/);
  assert.match(source,/bus\.mosi_io_num=ODYSSEY_SD_MOSI/);
  assert.match(source,/bus\.miso_io_num=ODYSSEY_SD_MISO/);
  assert.match(source,/ODYSSEY_SD_INIT_FREQ_KHZ=SDMMC_FREQ_PROBING/);
  assert.match(source,/ODYSSEY_SD_RUN_FREQ_KHZ=1000u/);
  assert.match(source,/host\.max_freq_khz=ODYSSEY_SD_INIT_FREQ_KHZ/);
  assert.match(source,/sdmmc_get_status\(card\)/);
  assert.match(source,/set_card_clk\(card->host\.slot,ODYSSEY_SD_RUN_FREQ_KHZ\)/);
  assert.match(source,/set_card_clk\(card->host\.slot,ODYSSEY_SD_INIT_FREQ_KHZ\)/);
  assert.doesNotMatch(source,/ODYSSEY_SD_MAX_FREQ_KHZ=10000u/);
  assert.match(source,/format_if_mount_failed=false/);
  assert.match(source,/allocation_unit_size=16\*1024/);
});

test('C3 SD lifecycle has one owner, bounded retries and complete cleanup',()=>{
  assert.match(source,/class OdysseySdGuard/);
  assert.match(source,/xSemaphoreCreateMutexStatic/);
  assert.match(source,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(source,/spi_bus_free\(SPI2_HOST\)/);
  assert.match(source,/ODYSSEY_SD_BOOT_ATTEMPTS=2/);
  assert.match(source,/ODYSSEY_SD_RECOVERY_ATTEMPTS=3/);
  assert.match(source,/ODYSSEY_SD_RETRY_BACKOFF_MS=250u/);
  assert.match(source,/gpio_reset_pin\(static_cast<gpio_num_t>\(ODYSSEY_SD_SCK\)\)/);
  assert.match(source,/gpio_set_pull_mode\(static_cast<gpio_num_t>\(ODYSSEY_SD_MISO\),GPIO_PULLUP_ONLY\)/);
  assert.match(source,/odysseySdMountLocked\("boot",ODYSSEY_SD_BOOT_ATTEMPTS\)/);
  assert.match(source,/odysseySdMountLocked\("recovery",ODYSSEY_SD_RECOVERY_ATTEMPTS\)/);
  assert.doesNotMatch(source,/odysseySdRawCommand|odysseySdProtocolProbe|odysseySdReadSectorZero/);
});

test('C3 runtime storage uses VFS/POSIX rather than Arduino SD/File',()=>{
  assert.match(recording,/fopen\(fullPath,"wb\+"\)/);
  assert.match(recording,/fwrite\(pcm,1,sizeof\(pcm\),file\)/);
  assert.match(recording,/fseek\(file,0,SEEK_SET\)/);
  assert.match(recording,/OdysseySdGuard storage/);
  assert.doesNotMatch(recording,/\bSD\.|\bFile\b/);

  assert.match(transfer,/opendir\(directoryPath\)/);
  assert.match(transfer,/fopen\(full,"rb"\)/);
  assert.match(transfer,/unlink\(full\)/);
  assert.match(transfer,/OdysseySdGuard guard/);
  assert.doesNotMatch(transfer,/\bSD\.|\bFile\b/);
});

test('C3 capability stages describe native storage lifecycle',()=>{
  assert.match(source,/probe 0=not checked, 1=SPI bus setup failed, 2=card protocol init failed/);
  assert.match(source,/3=FAT mount failed, 4=VFS validation failed, 6=ready/);
  assert.match(source,/odysseySdBootState=1;\n  odysseySdProbeStage=6/);
  assert.match(source,/result==ESP_FAIL\)\?3:2/);
});

test('device profile still emits original Odyssey pins and only C3 advertises SD sync',()=>{
  for(const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m']){
    const target=getTarget(id),profile=renderProfile(target);
    for(const [signal,pin] of Object.entries(target.hardware.sdDetection))
      assert(profile.includes(`#define SYNAP_SD_${signal.toUpperCase()}_PIN ${pin}`));
    assert.equal(target.features.includes('sd'),id==='esp32c3-supermini-4m');
  }
  assert(!renderProfile(getTarget('xiao-esp32s3-sense-8m')).includes('SYNAP_SD_'));
});

test('production build no longer patches the Arduino SD core',()=>{
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');
  assert.doesNotMatch(workflow,/patch-arduino-sd|SYNAP_ARDUINO_SD_SRC/);
  assert.equal(fs.existsSync(path.join(root,'tools/patch-arduino-sd.cjs')),false);
});

test('C3 preserves the native mount error and attempt count for BLE diagnostics',()=>{
  assert.match(source,/odysseySdLastMountError\{ESP_OK\}/);
  assert.match(source,/\+\+odysseySdMountAttempts/);
  assert.match(source,/odysseySdLastMountError=result==ESP_OK\?ESP_FAIL:result/);
  assert.match(source,/odysseySdLastMountError=ESP_OK;[\s\S]*odysseySdBootState=1/);
  assert.match(transfer,/odysseySdLastError\(\)/);
  assert.match(transfer,/odysseySdAttemptCount\(\)/);
  assert.match(transfer,/mountAttempts/);
});
