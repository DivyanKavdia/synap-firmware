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

test('C3 reproduces build-1445 first mount and isolates SPI with GPIO bitbang',()=>{
  const target=getTarget('esp32c3-supermini-4m');
  assert.deepEqual(target.hardware.sdDetection,{cs:0,sck:10,mosi:21,miso:20});
  assert.match(source,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(source,/ODYSSEY_SD_INIT_FREQ_HZ=400000u/);
  assert.match(source,/ODYSSEY_SD_MAX_OPEN_FILES=1/);
  assert.doesNotMatch(source,/ODYSSEY_SD_RESCUE_FREQ_HZ|ODYSSEY_SD_STARTUP_SETTLE_MS/);
  assert.match(source,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_INIT_FREQ_HZ,/);
  assert.match(source,/ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,false/);
  assert.match(source,/odysseySdBitBangTransfer/);
  assert.match(source,/odysseySdBitBangCommand\(0u,0u,0x95u\)/);
  assert.match(source,/odysseySdBitBangCommand\(8u,0x1AAu,0x87u/);
  assert.match(source,/digitalRead\(ODYSSEY_SD_MISO\)/);
  assert(source.indexOf('bool mounted=odysseySdBeginLocked();')<source.indexOf('odysseySdBitBangRecoverLocked(reason)'));
  assert.doesNotMatch(source,/odysseySdHoldBusIdleEarly|odysseySdRearmProtocolLocked|odysseySdStopWriteLocked/);
  assert.match(source,/if \(mounted\) markOdysseySdBatteryDividerPresent\(\)/);
  assert.match(boot,/OdysseyTransfer::initialize\(\);[\s\S]*odysseyInitializeSdCardBeforeBle\(\);/);
  assert.doesNotMatch(boot,/odysseySdHoldBusIdleEarly/);
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

test('C3 hard init failures expose direct GPIO probe and never format media',()=>{
  assert.match(source,/odysseySdLastMountError\{ESP_OK\}/);
  assert.match(source,/\+\+odysseySdMountAttempts/);
  assert.match(source,/odysseySdBootState=2;odysseySdProbeStage=2/);
  assert.match(source,/odysseySdBitBangCsHigh/);
  assert.match(source,/odysseySdBitBangCsLow/);
  assert.match(source,/pinMode\(ODYSSEY_SD_MISO,INPUT_PULLUP\)/);
  assert.match(source,/odysseySdBitBangStopWriteLocked/);
  assert.match(source,/CMD12 may be issued while CMD18 data is still flowing/);
  assert.match(source,/REQUIRED_IDLE_BYTES=64u/);
  assert.match(source,/MAX_DRAIN_BYTES=8192u/);
  assert.match(source,/odysseySdBitBangTransfer\(0xFD\)/);
  assert.match(source,/odysseySdBitBangStopReadLocked\(cmd12,drainBytes\)/);
  assert.match(source,/odysseySdBitBangCmd12/);
  assert.match(source,/odysseySdBitBangCmd0/);
  assert.match(source,/odysseySdBitBangCmd8/);
  assert.match(transfer,/bbHigh/);
  assert.match(transfer,/bbLow/);
  assert.match(transfer,/mountWhy/);
  assert.match(transfer,/bbStop/);
  assert.match(transfer,/bbCmd12Candidate/);
  assert.match(transfer,/bbReadIdle/);
  assert.match(transfer,/bbDrain/);
  assert.match(transfer,/bbCmd0/);
  assert.match(transfer,/bbCmd8/);
  assert.match(transfer,/bbR7/);
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
