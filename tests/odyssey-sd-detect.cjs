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
  assert.match(source,/ODYSSEY_SD_MAX_OPEN_FILES=4/);
  assert.match(source,/if \(!odysseySdSpi\.begin\(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS\)\)/);
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
  assert.match(source,/esp_vfs_fat_unregister_path\(ODYSSEY_SD_MOUNT_POINT\)/);
  assert.match(source,/residual!=ESP_ERR_INVALID_STATE/);
  assert.match(source,/odysseySdSpi\.end\(\)/);
  assert.match(source,/odysseySdReleaseLocked\(\)/);
  const release=source.split('static bool odysseySdReleaseLocked()')[1].split('static bool odysseySdBeginLocked')[0];
  assert.doesNotMatch(release,/if \(!odysseySdCloseReadLocked\(\)\) return false/);
  assert(release.indexOf('SD.end()')<release.indexOf('odysseySdHostMounted=false'),
    'host ownership must be cleared only after teardown is attempted');
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


test('C3 media operations distinguish missing content from FAT I/O failure',()=>{
  assert.match(transfer,/static int segmentedWavState/);
  assert.match(transfer,/journal<0\) return -1/);
  assert.match(transfer,/return errno==ENOENT\?FILE_UNAVAILABLE:IO_ERROR/);
  assert.match(transfer,/Verify both source objects are gone before acknowledging deletion/);
  assert.match(transfer,/static uint8_t clearRecordings\(uint32_t& removed\)/);
  assert.match(transfer,/if \(error==IO_ERROR\) odysseySdMarkVfsFailure\(\)/);
  assert.match(transfer,/ioErrno/);
  assert.match(transfer,/releaseErr/);
  assert.match(transfer,/releaseAttempts/);
});

test('C3 catalogue refuses silent truncation and clear drains every Synap recording in batches',()=>{
  assert.match(transfer,/if \(count>=100\) \{ catalogueErrno=EOVERFLOW;break; \}/);
  assert.match(transfer,/char batch\[16\]\[64\]\{\}/);
  assert.match(transfer,/for \(;;\) \{[\s\S]*DIR\* directory=opendir\(directoryPath\)/);
  assert.match(transfer,/static bool orphanJournalPath/);
  assert.match(transfer,/char journals\[16\]\[144\]\{\}/);
  assert.doesNotMatch(transfer,/String (?:logicalPaths|batch|journals)\[/);
  assert.match(transfer,/These are Synap-owned recovery metadata only/);
  assert.match(transfer,/catalogueBuffer\.reserve\(12288\)/);
});
