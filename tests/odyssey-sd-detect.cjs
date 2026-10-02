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

test('C3 restores proven Arduino SD SPI initialization on fixed Odyssey pins',()=>{
  const target=getTarget('esp32c3-supermini-4m');
  assert.deepEqual(target.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  assert.match(source,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(source,/ODYSSEY_SD_INIT_FREQ_HZ=400000u/);
  assert.match(source,/ODYSSEY_SD_RESCUE_FREQ_HZ=100000u/);
  assert.match(source,/ODYSSEY_SD_RESCUE_BUSY_MS=3000u/);
  assert.match(source,/ODYSSEY_SD_STARTUP_SETTLE_MS=3000u/);
  assert.match(source,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_INIT_FREQ_HZ,/);
  assert.match(source,/ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,false/);
  assert.match(source,/ODYSSEY_SD_BOOT_ATTEMPTS=1/);
  assert.match(source,/ODYSSEY_SD_RECOVERY_ATTEMPTS=1/);
  assert.doesNotMatch(source,/esp_vfs_fat_sdspi_mount|spi_bus_initialize|gpio_reset_pin|gpio_set_pull_mode/);
  assert.match(source,/odysseySdWaitReadyLocked\(500u,readyByte\)/);
  assert.match(source,/lastByte==0xFF/);
  assert.match(source,/odysseySdLastCsHighByte/);
  assert.match(source,/csHighByte=odysseySdSpi\.transfer\(0xFF\)/);
  assert.match(source,/odysseySdStopWriteLocked\(ODYSSEY_SD_RESCUE_BUSY_MS,rescueByte\)/);
  assert.match(source,/odysseySdCommandLocked\(12u,0u,0x61u,true\)/);
  assert.match(source,/odysseySdWaitReadyLocked\(500u,cmd12Byte\)/);
  assert.match(source,/odysseySdCommandLocked\(0u,0u,0x95u,false\)/);
  assert.doesNotMatch(source,/if \(!ready\)[\s\S]*continue;/);
  assert.match(source,/response==0x00 \|\| response==0x01\) markOdysseySdBatteryDividerPresent\(\)/);
  assert.match(source,/if \(mounted\) markOdysseySdBatteryDividerPresent\(\)/);
  assert.match(source,/odysseyWaitForSdStartupSettle\(\);[\s\S]*OdysseySdGuard guard/);
  assert.match(boot,/odysseyInitializeSdCardBeforeBle\(\);[\s\S]*if \(odysseySdBatteryDividerPresent\(\)\) sampleBattery\(true\);/);
});

test('C3 retains current guarded POSIX recording and verified sync runtime',()=>{
  assert.match(source,/class OdysseySdGuard/);
  assert.match(source,/xSemaphoreCreateMutexStatic/);
  assert.match(source,/SD\.end\(\)/);
  assert.match(source,/odysseySdSpi\.end\(\)/);
  assert.match(source,/odysseySdValidateVfsLocked/);
  assert.match(source,/opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(source,/\.synap-media-probe\.tmp/);
  assert.match(recording,/fopen\(fullPath,"wb\+"\)/);
  assert.match(recording,/OdysseySdGuard storage/);
  assert.match(transfer,/opendir\(directoryPath\)/);
  assert.match(transfer,/fopen\(full,"rb"\)/);
  assert.match(transfer,/unlink\(full\)/);
});

test('C3 hard init failures retain BLE diagnostics and never format media',()=>{
  assert.match(source,/odysseySdLastMountError\{ESP_OK\}/);
  assert.match(source,/\+\+odysseySdMountAttempts/);
  assert.match(source,/odysseySdBootState=2;odysseySdProbeStage=2/);
  assert.match(source,/odysseySdLastMountError=ESP_FAIL/);
  assert.match(source,/odysseySdLastMountError=ESP_OK;[\s\S]*odysseySdBootState=1/);
  assert.match(transfer,/odysseySdLastError\(\)/);
  assert.match(transfer,/odysseySdAttemptCount\(\)/);
  assert.match(transfer,/mountAttempts/);
  assert.match(transfer,/cmd12/);
  assert.match(transfer,/cmd12Ready/);
  assert.match(transfer,/rescueReady/);
  assert.match(transfer,/cmdReady/);
  assert.match(transfer,/odysseySdLastCmd12Response\(\)/);
  assert.match(transfer,/odysseySdLastCmd12ReadyState\(\)/);
  assert.match(transfer,/odysseySdLastRescueReadyState\(\)/);
  assert.match(transfer,/odysseySdLastCmdReadyState\(\)/);
  assert.doesNotMatch(source,/format_if_mount_failed=true/);
});

test('production build preserves the stock Arduino 3.3.5 SD initializer used by build 1445',()=>{
  assert.match(workflow,/arduino-cli core install esp32:esp32@3\.3\.5/);
  assert.doesNotMatch(workflow,/patch-arduino-sd\.cjs|SYNAP_ARDUINO_SD_SRC/);
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
