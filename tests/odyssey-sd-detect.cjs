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


test('C3 uses the proven Arduino SPI host with the locked device pins',()=>{
  const target=getTarget('esp32c3-supermini-4m');
  assert.deepEqual(target.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  assert.match(source,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(source,/ODYSSEY_SD_DATA_FREQ_HZ=400000u/);
  assert.match(source,/ODYSSEY_SD_STARTUP_SETTLE_MS=3000u/);
  assert.match(source,/ODYSSEY_SD_MAX_OPEN_FILES=1/);
  assert.match(source,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,/);
  assert.doesNotMatch(source,/SDSPI_HOST_DEFAULT|esp_vfs_fat_sdspi_mount|spi_bus_initialize/);
  assert.match(source,/esp_vfs_fat_create_contiguous_file\(/);
  assert.match(source,/odysseySdRecoverRecordingPartsLocked\(\)/);
  assert.match(source,/now<ODYSSEY_SD_STARTUP_SETTLE_MS/);
  assert.match(boot,/OdysseyTransfer::initialize\(\);[\s\S]*odysseyInitializeSdCardBeforeBle\(\);/);
  assert.match(boot,/odysseyInitializeSdCardBeforeBle\(\);[\s\S]*if \(odysseySdBatteryDividerPresent\(\)\) sampleBattery\(true\);/);
});

test('C3 VFS has one guarded owner and an explicit Arduino host release lifecycle',()=>{
  assert.match(source,/xSemaphoreCreateMutexStatic/);
  assert.match(source,/SD\.end\(\)/);
  assert.match(source,/odysseySdSpi\.end\(\)/);
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

test('C3 only formats on explicit request while normal mount stays read-safe',()=>{
  const normalMount=source.split('static bool odysseySdMountOnceLocked')[1].split('static bool odysseySdMountLocked')[0];
  const explicitFormat=source.split('bool odysseyFormatSdCard()')[1].split('bool odysseyPrepareSdForPowerTransition')[0];
  assert.match(normalMount,/odysseySdBeginLocked\(\)/);
  assert.doesNotMatch(normalMount,/odysseySdBeginLocked\(true\)|writeRAW|format/i);
  assert.match(explicitFormat,/SD\.writeRAW\(blankSector,0\)/);
  assert.match(explicitFormat,/odysseySdBeginLocked\(true\)/);
  assert.match(explicitFormat,/odysseySdValidateVfsLocked/);
  assert.match(transfer,/\\"stage\\":\\"catalogue\\"/);
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
