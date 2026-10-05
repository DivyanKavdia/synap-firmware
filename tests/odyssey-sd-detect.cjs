'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {getTarget}=require('../tools/targets.cjs');
const {renderProfile}=require('../tools/device-profile.cjs');
const root=path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-detect.cpp'),'utf8');
const recording=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-recording.cpp'),'utf8');
const transfer=fs.readFileSync(path.join(root,'firmware/shared/odyssey-sd-transfer.cpp'),'utf8');
const boot=fs.readFileSync(path.join(root,'firmware/shared/boot.cpp'),'utf8');
const workflow=fs.readFileSync(path.join(root,'.github/workflows/firmware.yml'),'utf8');

test('C3 uses one native IDF SDSPI host with the locked device pins',()=>{
  const target=getTarget('esp32c3-supermini-4m');
  assert.deepEqual(target.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  assert.match(source,/SDSPI_HOST_DEFAULT\(\)/);
  assert.match(source,/ODYSSEY_SD_MAX_FREQ_KHZ=1000u/);
  assert.match(source,/ODYSSEY_SD_STARTUP_SETTLE_MS=3000u/);
  assert.match(source,/SDSPI_DEVICE_CONFIG_DEFAULT\(\)/);
  assert.match(source,/spi_bus_initialize\(SPI2_HOST,&bus,SDSPI_DEFAULT_DMA\)/);
  assert.match(source,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT/);
  assert.match(source,/slot\.host_id=SPI2_HOST/);
  assert.match(source,/slot\.gpio_cs=static_cast<gpio_num_t>\(ODYSSEY_SD_CS\)/);
  assert.match(source,/config\.format_if_mount_failed=false/);
  assert.match(source,/esp_vfs_fat_create_contiguous_file\(/);
  assert.match(source,/odysseySdRecoverRecordingPartsLocked\(\)/);
  assert.match(source,/now<ODYSSEY_SD_STARTUP_SETTLE_MS/);
  assert.match(boot,/OdysseyTransfer::initialize\(\);[\s\S]*odysseyInitializeSdCardBeforeBle\(\);/);
  assert.match(boot,/odysseyInitializeSdCardBeforeBle\(\);[\s\S]*if \(odysseySdBatteryDividerPresent\(\)\) sampleBattery\(true\);/);
});

test('C3 VFS has a single guarded owner and explicit checked unmount lifecycle',()=>{
  assert.match(source,/xSemaphoreCreateMutexStatic/);
  assert.match(source,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(source,/spi_bus_free\(SPI2_HOST\)/);
  assert.match(source,/odysseySdReleaseLocked\(\)/);
  assert.match(source,/bool odysseyPrepareSdForPowerTransition/);
  assert.match(source,/odysseyRecording\.load\(\)/);
  assert.match(source,/odysseySdValidateVfsLocked/);
  assert.match(source,/opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(source,/fsync\(fileno\(probe\)\)/);
  assert.match(recording,/OdysseySdGuard storage/);
  assert.match(recording,/open\(fullPath,O_RDWR\)/);
  assert.match(transfer,/OdysseySdGuard guard/);
  assert.match(transfer,/opendir\(directoryPath\)/);
  assert.match(transfer,/open\(full,O_RDONLY\)/);
  assert.match(transfer,/unlink\(full\)/);
});

test('C3 only formats on explicit request and recovery does not bitbang SD commands',()=>{
  const normalMount=source.split('static bool odysseySdMountOnceLocked')[1].split('static bool odysseySdMountLocked')[0];
  const explicitFormat=source.split('bool odysseyFormatSdCard()')[1].split('bool odysseyPrepareSdForPowerTransition')[0];
  assert.match(normalMount,/odysseySdBeginLocked\(\)/);
  assert.doesNotMatch(normalMount,/format|f_mkfs|sdcard_format/i);
  assert.match(source,/config\.format_if_mount_failed=formatIfMountFailed/);
  assert.match(explicitFormat,/odysseySdBeginLocked\(true\)/);
  assert.match(explicitFormat,/esp_vfs_fat_sdcard_format\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(explicitFormat,/odysseySdValidateVfsLocked/);
  assert.doesNotMatch(source,/writeRAW|BitBang|digitalRead\(ODYSSEY_SD_MISO\)/);
  assert.match(transfer,/\\"stage\\":\\"catalogue\\"/);
  assert.doesNotMatch(transfer,/bbCmd0|rawFF|bbCmd12/);
});

test('the C3-only backend leaves the S3 Arduino detection path and supported targets unchanged',()=>{
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  const s3=source.split('// Odyssey S3 remains detection-only')[1];
  assert.match(s3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,400000/);
  for(const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m']){
    const target=getTarget(id),profile=renderProfile(target);
    for(const [signal,pin] of Object.entries(target.hardware.sdDetection))
      assert(profile.includes(`#define SYNAP_SD_${signal.toUpperCase()}_PIN ${pin}`));
    assert.equal(target.features.includes('sd'),id==='esp32c3-supermini-4m');
  }
  assert(!renderProfile(getTarget('xiao-esp32s3-sense-8m')).includes('SYNAP_SD_'));
});
