'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..'),read=p=>fs.readFileSync(path.join(root,p),'utf8');
test('C3 SD mutex is released before a recording task deletes itself',()=>{
  const source=read('firmware/shared/odyssey-sd-recording.cpp');
  const take=source.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
  const task=source.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.match(take,/OdysseySdGuard storage;/);
  assert.doesNotMatch(take,/vTaskDelete/);
  assert.match(task,/odysseyRecordTake\(\);[\s\S]*odysseyRecording=false;[\s\S]*vTaskDelete\(nullptr\)/);
});
test('C3 reconnect never stops SD capture; START performs explicit handoff',()=>{
  const ble=read('firmware/shared/ble-control.cpp');
  const connect=ble.split('void onConnect(BLEServer* server) override {')[1].split('void onDisconnect(BLEServer* server) override {')[0];
  assert.doesNotMatch(connect,/odysseyStopRequested\s*=\s*true/);
  const session=read('firmware/shared/audio-session.cpp');
  assert.match(session,/odysseyPrepareForConnectedStreaming\(1500u\)/);
});
test('C3 keeps connected reads observational while offline double-tap owns bounded recovery',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.doesNotMatch(transfer,/automaticRetries|ODYSSEY_SD_REARM_STEPS|scheduled re-arm/);
  assert.match(detect,/odysseySdMountLocked\("boot",3\)/);
  assert.match(detect,/const uint8_t attempts=\(!strcmp\(why,"touch"\) \|\| !strcmp\(why,"post-record"\)\)\?2u:1u/);
  assert.match(recorder,/one-gesture offline start: recovering storage before capture/);
  assert.match(recorder,/odysseyRecoverSdCard\("touch"\)/);
  assert.match(recorder,/odysseyRecoverSdCard\("post-record"\)/);
  assert.match(transfer,/case 14:[\s\S]*odysseyRecoverSdCard\("op14"\)/);
  const readCase=transfer.split('case 4:')[1].split('case 7:')[0];
  assert.doesNotMatch(readCase,/odysseyRecoverSdCard\(/);
});
test('BLE STOP and reconnect cannot release SD-owned I2S or block the control task',()=>{
  const session=read('firmware/shared/audio-session.cpp');
  const stop=session.split('void stopStreaming(ErrorCode reason) {')[1].split('bool configureTransportFromPeerMtu() {')[0];
  assert.match(stop,/if \(!odysseyRecording\.load\(\)\) stopMicrophone\(\)/);
  assert.match(stop,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU/);
});

test('validated C3 OTA begin gracefully seals an active SD WAV before flashing',()=>{
  const source=read('firmware/shared/ota.cpp');
  const control=source.split('void otaTick() {')[1];
  const preflight=control.split('otaSession.packet(message.data,message.length')[0];
  assert.match(preflight,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU/);
  assert.match(preflight,/message.length==59 && message.data\[0\]==1/);
  assert.match(preflight,/!streamingEnabled.load\(\)/);
  assert.match(preflight,/odysseyRecording.load\(\) && !batteryCritical\(\)/);
  assert.match(preflight,/otaSession.state==Synap::AVAILABLE/);
  assert.match(preflight,/otaBackend.matchesDevice\(message.data\+41\)/);
  assert.match(preflight,/odysseyPrepareForConnectedStreaming\(2500u\)/);
  assert.match(control,/otaSession.packet\(message.data,message.length[\s\S]*?\|\| odysseyRecording.load\(\)/);
  assert(preflight.indexOf('odysseyPrepareForConnectedStreaming(2500u)')<control.indexOf('otaSession.packet(message.data,message.length'),'SD recording must be sealed before OTA begin');
});


test('C3 offline recording has visible purple heartbeat and failed-start feedback',()=>{
  const sketch=read('synap_esp32s3/synap_esp32s3.ino');
  const led=read('firmware/shared/status-led.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  assert.match(sketch,/odysseyRecordingStartedAt\{0\}, odysseyRecordFaultAt\{0\}/);
  assert.match(led,/now-odysseyRecordingStartedAt\.load\(\)\)%1800u/);
  assert.match(led,/phase<260u\) \{ r=LED_DIM\+4; b=LED_DIM\+6;/);
  assert.match(led,/uint32_t\(now-odysseyRecordFaultAt\.load\(\)\)<6000u/);
  assert.match(led,/phase<140u \|\| \(phase>=260u && phase<400u\)/);
  assert.match(recorder,/odysseyRecordingStartedAt=millis\(\);[\s\S]*?odysseyRecording=true/);
  assert.match(recorder,/if \(failed \|\| totalBytes==0\) odysseyRecordFaultAt=millis\(\)/);
  assert.match(led,/odysseySdRecoveryActive\.load\(\)/);
  assert.match(recorder,/double tap -> SD recover \+ audio START/);
});


test('C3 SD readiness validates directory, durable write and read-back before publishing ready',()=>{
  const sd=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(sd,/DIR\* verified=opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(sd,/FILE\* probe=fopen\(probePath,"wb"\)/);
  assert.match(sd,/fsync\(fileno\(probe\)\)/);
  assert.match(sd,/fread\(readback,1,sizeof\(readback\),verify\)/);
  assert.match(sd,/memcmp\(readback,"SD",sizeof\(readback\)\)/);
  assert.match(sd,/ODYSSEY_SD_DATA_FREQ_HZ=400000u/);
  assert.match(sd,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(sd,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,/);
  assert.doesNotMatch(sd,/esp_vfs_fat_sdspi_mount|spi_bus_initialize/);
  assert.match(sd,/esp_vfs_fat_create_contiguous_file\(/);
  assert.match(sd,/odysseySdRecoverRecordingPartsLocked\(\)/);
});
test('C3 catalogue failure is observational and never auto-remounts',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(transfer,/case 7:[\s\S]*?error=catalogue\(total\)/);
  const catalogueCase=transfer.split('case 7:')[1].split('case 8:')[0];
  assert.doesNotMatch(catalogueCase,/odysseyRecoverSdCard\(/);
  assert.match(catalogueCase,/errno=catalogueErrno\?catalogueErrno:\(errno\?errno:EIO\);odysseySdMarkVfsFailure\(\)/);
  assert.match(transfer,/case 14:[\s\S]*odysseyRecoverSdCard\("op14"\)/);
  assert.match(transfer,/sdProbe/);
});
test('C3 PWA connection is acknowledged by three visible green flashes',()=>{
  const led=read('firmware/shared/status-led.cpp'),ble=read('firmware/shared/ble-control.cpp');
  assert.match(ble,/deviceConnected\.store\(true\);\s*connectedLedAt=millis\(\);/);
  assert.match(led,/elapsed<1500u && elapsed%500u<180u\) g=LED_DIM\+5/);
});


test('C3 failed mount never formats implicitly and still reports diagnostics',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const normalMount=detect.split('static bool odysseySdMountOnceLocked')[1].split('static bool odysseySdMountLocked')[0];
  const explicitFormat=detect.split('bool odysseyFormatSdCard()')[1].split('bool odysseyPrepareSdForPowerTransition')[0];
  assert.match(normalMount,/odysseySdBeginLocked\(\)/);
  assert.doesNotMatch(normalMount,/odysseySdBeginLocked\(true\)/);
  assert.match(explicitFormat,/SD\.writeRAW\(blankSector,0\)/);
  assert.match(explicitFormat,/odysseySdBeginLocked\(true\)/);
  assert.match(detect,/odysseySdProbeStage=formatIfMountFailed\?3:2/);
  assert.match(transfer,/espErr/);
  assert.match(transfer,/mountAttempts/);
});

test('C3 uses the proven Arduino SPI mount while retaining guarded VFS diagnostics',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const c3=detect.split('// Odyssey S3 remains detection-only')[0];
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(c3,/static SPIClass odysseySdSpi\(FSPI\)/);
  assert.match(c3,/ODYSSEY_SD_DATA_FREQ_HZ=400000u/);
  assert.match(c3,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,/);
  assert.match(c3,/SD\.end\(\)/);
  assert.match(c3,/odysseySdSpi\.end\(\)/);
  assert.doesNotMatch(c3,/esp_vfs_fat_sdspi_mount|spi_bus_initialize|SDSPI_HOST_DEFAULT/);
  assert.match(transfer,/espErr/);
  assert.match(transfer,/recordStage/);
});

test('C3 releases the Arduino SD host before OTA reboot, app restart and deep sleep',()=>{
  const runtime=read('firmware/shared/runtime.cpp');
  const ota=read('firmware/shared/ota.cpp');
  const ble=read('firmware/shared/ble-control.cpp');
  const power=read('firmware/shared/power.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(runtime,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(detect,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  const transition=detect.split('bool odysseyPrepareSdForPowerTransition')[1];
  assert.match(transition,/odysseySdReleaseLocked\(\)/);
  assert.match(transition,/if \(odysseyRecording\.load\(\)\)[\s\S]*return false/);
  assert.match(detect,/SD\.end\(\)/);
  assert.match(detect,/esp_vfs_fat_unregister_path\(ODYSSEY_SD_MOUNT_POINT\)/);
  assert.match(detect,/odysseySdSpi\.end\(\)/);
  assert.doesNotMatch(detect,/esp_vfs_fat_sdcard_unmount|spi_bus_free/);
  assert.match(ota,/otaSession\.state==Synap::COMMITTED[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(ble,/CMD_RESTART:[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(power,/entering deep sleep request=[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*esp_deep_sleep_start\(\)/);
});

test('C3 runtime VFS failure retains Arduino host ownership until explicit release',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(detect,/std::atomic<bool> odysseySdHostMounted\{false\}/);
  assert.match(detect,/odysseySdHostMounted=true;[\s\S]*markOdysseySdBatteryDividerPresent\(\)/);
  const release=detect.split('static bool odysseySdReleaseLocked()')[1].split('static bool odysseySdBeginLocked')[0];
  assert.match(release,/SD\.end\(\)[\s\S]*odysseySdSpi\.end\(\)[\s\S]*odysseySdHostMounted=false/);
  assert.match(detect,/const bool wasReady=odysseySdReady\(\)/);
});
test('C3 offline failure telemetry identifies write stage and persisted bytes',()=>{
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(recorder,/odysseyRecordFailureStage/);
  assert.match(recorder,/failureStage=failure\(5\)/);
  assert.match(recorder,/failureStage=failure\(6\)/);
  assert.match(recorder,/failureStage=failure\(7\)/);
  assert.match(transfer,/\\\"recordStage\\\":%u/);
  assert.match(transfer,/\\\"recordBytes\\\":%lu/);
});


test('C3 mount validates custom SPI startup and card geometry before publishing ready',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(detect,/if \(!odysseySdSpi\.begin\(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS\)\)/);
  assert.match(detect,/const uint64_t cardBytes=SD\.cardSize\(\)/);
  assert.match(detect,/sectorBytes!=512u/);
  assert.match(detect,/odysseySdValidateVfsLocked\(reason,attempt\)/);
});

test('C3 recovery performs one checked teardown and successful takes clear stale failure evidence',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const recovery=detect.split('bool odysseyRecoverSdCard(const char* reason)')[1].split('bool odysseyFormatSdCard()')[0];
  assert.doesNotMatch(recovery,/odysseySdReleaseLocked\(\)/);
  assert.match(recovery,/const uint8_t attempts=/);
  assert.match(recovery,/odysseySdMountLocked\(why,attempts\)/);
  assert.match(recorder,/else if \(totalBytes\) odysseySaveRecordFailure\(0,0,0\)/);
  assert.match(recorder,/errno=firstErrno\?firstErrno:EIO;odysseySdMarkVfsFailure\(\)/);
});

test('C3 sync-source deletion is idempotent and keeps the journal until the WAV is gone',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const remove=transfer.split('static uint8_t removeFileLocked(const char* path) {')[1].split('static uint8_t removeFile(const char* path) {')[0];
  assert(remove.indexOf('unlink(full)')<remove.indexOf('odysseyRemoveJournal(full)'));
  assert.match(remove,/statResult!=0 && errno!=ENOENT/);
  assert.match(remove,/Verify WAV, journal and integrity metadata are all gone before acknowledging deletion/);
  assert.match(remove,/odysseyJournalPresence\(full\)/);
});

test('C3 offline V2 guards free space and persists per-WAV session integrity metadata',()=>{
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const io=read('firmware/shared/odyssey-sd-io.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(recorder,/ODYSSEY_SD_FREE_RESERVE_BYTES=2ull\*1024ull\*1024ull/);
  assert.match(recorder,/failureStage=failure\(31\)/);
  assert.match(recorder,/odysseySdCrcUpdate\(segmentCrcState/);
  assert.match(recorder,/odysseyWriteWavMeta\(fullPath,takeHigh,takeLow,segment/);
  assert.match(io,/SYNAPM01/);
  assert.match(io,/struct OdysseyWavMeta/);
  assert.match(transfer,/crc32/);
  assert.match(transfer,/take/);
  assert.match(transfer,/part/);
  assert.match(transfer,/pcmBytes/);
  assert.match(transfer,/freeBytes/);
});

test('C3 warm boot mount retries are bounded and progressively settled',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(detect,/odysseySdMountLocked\("boot",3\)/);
  assert.match(detect,/retryDelayMs=250u\*uint32_t\(attempt\)\*uint32_t\(attempt\)/);
  assert.match(detect,/Every retry starts from a full/);
});

test('C3 offline LED truth separates preparation from confirmed PCM capture',()=>{
  const runtime=read('firmware/shared/runtime.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const led=read('firmware/shared/status-led.cpp');
  assert.match(runtime,/odysseyCaptureActive/);
  assert.match(led,/else if \(odysseyCaptureActive\.load\(\)\)/);
  assert.doesNotMatch(led,/else if \(odysseyRecording\.load\(\)\)/);
  assert.match(recorder,/odysseySdRecoveryActive=true;[\s\S]*xTaskCreate/);
  assert.match(recorder,/startMicrophone\(\)[\s\S]*odysseyCaptureActive=true;[\s\S]*odysseySdRecoveryActive=false/);
  assert.match(recorder,/odysseyCaptureActive=false;[\s\S]*stopMicrophone\(\)/);
});

test('C3 diagnostics preserve root SD failure when recovery later collapses to ENODEV',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(detect,/void odysseySaveRootStorageFailure/);
  assert.match(detect,/stage==1 \|\| stage==4 \|\| stage==8 \|\| stage==31/);
  assert.match(recorder,/odysseySaveRootStorageFailure\(failureStage,firstErrno/);
  assert.match(transfer,/rootRecordStage/);
  assert.match(transfer,/rootRecordErrno/);
  assert.match(transfer,/lastGoodFreeBytes/);
  assert.match(detect,/odysseySdHostMounted=false;[\s\S]*odysseySdLastFreeBytes=0/);
});
