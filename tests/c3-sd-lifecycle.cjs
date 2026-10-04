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
test('C3 remounts only on explicit op14 or physical touch',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.doesNotMatch(transfer,/automaticRetries|background mount retry|odysseySdConsumeAutoRearm|scheduled re-arm/);
  assert.doesNotMatch(detect,/ODYSSEY_SD_REARM_STEPS|odysseySdArmBootRearm|odysseySdConsumeAutoRearm/);
  assert.match(detect,/odysseySdMountLocked\("boot",1\)/);
  assert.match(detect,/odysseySdMountLocked\(reason\?reason:"op14",1\)/);
  assert.match(transfer,/physical touch requested software recovery/);
  assert.match(transfer,/odysseyRecoverSdCard\("touch"\)/);
  assert.match(transfer,/case 14:[\s\S]*odysseyRecoverSdCard\("op14"\)/);
  const readCase=transfer.split('case 4:')[1].split('case 7:')[0];
  assert.doesNotMatch(readCase,/odysseySdRequestRecovery\(/);
  assert.match(recorder,/odysseySdRequestRecovery\(\)/);
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
  assert.match(recorder,/if \(failed \|\| bytes==0\) odysseyRecordFaultAt=millis\(\)/);
  assert.match(recorder,/odysseySdRequestRecovery\(\);\s*odysseyRecordFaultAt=millis\(\)/);
});

test('C3 SD readiness validates directory, durable write and read-back before publishing ready',()=>{
  const sd=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(sd,/DIR\* verified=opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(sd,/FILE\* probe=fopen\(probePath,"wb"\)/);
  assert.match(sd,/fsync\(fileno\(probe\)\)/);
  assert.match(sd,/fread\(readback,1,sizeof\(readback\),verify\)/);
  assert.match(sd,/memcmp\(readback,"SD",sizeof\(readback\)\)/);
  assert(sd.indexOf('DIR* verified=opendir')<sd.indexOf('odysseySdLastMountError=ESP_OK;\n  odysseySdBootState=1;odysseySdProbeStage=6;'));
  assert.match(sd,/ODYSSEY_SD_MAX_FREQ_KHZ=4000u/);
  assert.match(sd,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT/);
});
test('C3 catalogue failure is observational and never auto-remounts',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(transfer,/case 7:[\s\S]*?error=catalogue\(total\)/);
  const catalogueCase=transfer.split('case 7:')[1].split('case 8:')[0];
  assert.doesNotMatch(catalogueCase,/odysseyRecoverSdCard\(/);
  assert.match(catalogueCase,/odysseySdMarkVfsFailure\(\)/);
  assert.match(transfer,/case 14:[\s\S]*odysseyRecoverSdCard\("op14"\)/);
  assert.match(transfer,/sdProbe/);
});
test('C3 PWA connection is acknowledged by three visible green flashes',()=>{
  const led=read('firmware/shared/status-led.cpp'),ble=read('firmware/shared/ble-control.cpp');
  assert.match(ble,/deviceConnected\.store\(true\);\s*connectedLedAt=millis\(\);/);
  assert.match(led,/elapsed<1500u && elapsed%500u<180u\) g=LED_DIM\+5/);
});

test('C3 failed mount never formats implicitly and still reports IDF diagnostics',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const normalMount=detect.split('static bool odysseySdMountOnceLocked')[1].split('static bool odysseySdMountLocked')[0];
  const explicitFormat=detect.split('bool odysseyFormatSdCard()')[1].split('bool odysseyPrepareSdForPowerTransition')[0];
  assert.match(normalMount,/odysseySdBeginLocked\(\)/);
  assert.doesNotMatch(normalMount,/odysseySdBeginLocked\(true\)/);
  assert.match(explicitFormat,/odysseySdBeginLocked\(true\)/);
  assert.match(detect,/config\.format_if_mount_failed=formatIfMountFailed/);
  assert.match(detect,/config\.format_if_mount_failed=false/);
  assert.match(detect,/esp_vfs_fat_sdcard_format\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(detect,/odysseySdLastMountError=ESP_FAIL/);
  assert.match(detect,/odysseySdProbeStage=\(err==ESP_ERR_NOT_FOUND\)\?0:2/);
  assert.match(transfer,/espErr/);
  assert.match(transfer,/mountAttempts/);
});


test('C3 uses only the IDF SDSPI driver and never bitbangs around its state machine',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const c3=detect.split('// Odyssey S3 remains detection-only')[0];
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(detect,/spi_bus_initialize\(SPI2_HOST,&bus,SDSPI_DEFAULT_DMA\)/);
  assert.match(detect,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT/);
  assert.match(detect,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(detect,/spi_bus_free\(SPI2_HOST\)/);
  assert.doesNotMatch(c3,/SPIClass|SD\.begin|SD\.end|BitBang|digitalRead\(ODYSSEY_SD_MISO\)/);
  assert.doesNotMatch(transfer,/bbHigh|rawFF|bbCmd12|bbCmd0/);
  assert.match(transfer,/espErr/);
  assert.match(transfer,/recordStage/);
});
test('C3 unmounts FatFs before OTA reboot, app restart and deep sleep',()=>{
  const runtime=read('firmware/shared/runtime.cpp');
  const ota=read('firmware/shared/ota.cpp');
  const ble=read('firmware/shared/ble-control.cpp');
  const power=read('firmware/shared/power.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(runtime,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(detect,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  const transition=detect.split('bool odysseyPrepareSdForPowerTransition')[1];
  assert.match(transition,/odysseySdReleaseLocked\(\)/);
  assert.match(transition,/odysseySdReleaseLocked\(\)/);
  assert.match(transition,/if \(odysseyRecording\.load\(\)\)[\s\S]*return false/);
  assert.match(detect,/esp_vfs_fat_sdcard_unmount\(ODYSSEY_SD_MOUNT_POINT,odysseySdCard\)/);
  assert.match(detect,/spi_bus_free\(SPI2_HOST\)/);
  assert.match(detect,/if \(err!=ESP_OK\)[\s\S]*keeping SPI bus owned/);
  assert.match(ota,/otaSession\.state==Synap::COMMITTED[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(ble,/CMD_RESTART:[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(power,/entering deep sleep request=[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*esp_deep_sleep_start\(\)/);
});

test('C3 runtime VFS failure retains driver ownership until checked unmount',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(detect,/std::atomic<bool> odysseySdHostMounted\{false\}/);
  assert.match(detect,/odysseySdHostMounted=true;[\s\S]*markOdysseySdBatteryDividerPresent\(\)/);
  assert.match(detect,/if \(odysseySdHostMounted\.load\(\)\)[\s\S]*esp_vfs_fat_sdcard_unmount/);
  assert.match(detect,/if \(err!=ESP_OK\)[\s\S]*keeping SPI bus owned/);
  assert.match(detect,/const bool wasReady=odysseySdReady\(\)/);
});
test('C3 offline failure telemetry identifies write stage and persisted bytes',()=>{
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(recorder,/odysseyRecordFailureStage/);
  assert.match(recorder,/failureStage=5/);
  assert.match(recorder,/failureStage=6/);
  assert.match(recorder,/failureStage=7/);
  assert.match(transfer,/\\\"recordStage\\\":%u/);
  assert.match(transfer,/\\\"recordBytes\\\":%lu/);
});
