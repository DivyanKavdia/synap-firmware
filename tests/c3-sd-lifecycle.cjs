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
test('C3 supports background mounting without remounting under active capture',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  assert.match(detect,/odysseySdRequestRecovery\(\)/);
  assert.match(transfer,/automaticRetries<3/);
  assert.match(transfer,/!odysseyRecording\.load\(\)/);
  assert.match(transfer,/!streamingEnabled\.load\(\)/);
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

test('C3 SD readiness validates directory and writable media before publishing ready',()=>{
  const sd=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(sd,/DIR\* verified=opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(sd,/FILE\* probe=fopen\(probePath,"wb"\)/);
  assert.match(sd,/!writeOk \|\| !closeOk \|\| !removeOk/);
  assert(sd.indexOf('DIR* verified=opendir')<sd.indexOf('odysseySdBootState=1;\n  odysseySdProbeStage=6;'));
  assert.match(sd,/ODYSSEY_SD_INIT_FREQ_HZ=400000u/);
});
test('C3 SD catalogue I/O recovery never tears down live recording and reports errno',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(transfer,/case 7:[\s\S]*?error=catalogue\(total\)/);
  assert.match(transfer,/odysseySdUseProbingClock\(\)/);
  assert.match(transfer,/if \(odysseyRecoverSdCard\(\)\) error=catalogue\(total\)/);
  assert.match(transfer,/if \(error==IO_ERROR\) odysseySdMarkVfsFailure\(\)/);
  assert.match(transfer,/sdProbe/);
  assert.match(transfer,/\(requested \|\| !odysseySdReady\(\)\)/);
  const guard=transfer.split('if (odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending)')[1];
  assert(guard.indexOf('case 7:')>0 && guard.indexOf('odysseyRecoverSdCard()')>guard.indexOf('case 7:'));
});

test('C3 PWA connection is acknowledged by three visible green flashes',()=>{
  const led=read('firmware/shared/status-led.cpp'),ble=read('firmware/shared/ble-control.cpp');
  assert.match(ble,/deviceConnected\.store\(true\);\s*connectedLedAt=millis\(\);/);
  assert.match(led,/elapsed<1500u && elapsed%500u<180u\) g=LED_DIM\+5/);
});

test('C3 failed mount reports compatibility diagnostics without formatting or deleting WAVs',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(detect,/ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,false/);
  assert.match(detect,/odysseySdLastMountError=ESP_FAIL/);
  assert.match(detect,/odysseySdBootState=2;odysseySdProbeStage=2/);
  assert.match(transfer,/espErr/);
  assert.match(transfer,/mountAttempts/);
});


test('C3 re-arms a still-powered SD card and reports the raw CMD0 response',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(detect,/for \(uint8_t i=0;i<20;\+\+i\) odysseySdSpi\.transfer\(0xFF\)/);
  assert.match(detect,/odysseySdSpi\.transfer\(0x40\);/);
  assert.match(detect,/attempt<2 && response!=0x01/);
  assert.match(detect,/mounted=odysseySdBeginLocked\(\);[\s\S]*odysseySdRearmProtocolLocked\(reason\)[\s\S]*mounted=odysseySdBeginLocked\(\)/);
  assert.match(detect,/odysseySdBeginAttempts/);
  assert.match(detect,/odysseySdLastCmd0/);
  assert.match(detect,/odysseySdLastCsHighByte/);
  assert.match(detect,/odysseySdWaitReadyLocked\(500u,readyByte\)[\s\S]*odysseySdSpi\.transfer\(0x40\)/);
  assert.match(transfer,/beginAttempts/);
  assert.match(transfer,/csHigh/);
  assert.match(transfer,/cmd0/);
});

test('C3 quiesces SD before OTA reboot, app restart and deep sleep',()=>{
  const runtime=read('firmware/shared/runtime.cpp');
  const ota=read('firmware/shared/ota.cpp');
  const ble=read('firmware/shared/ble-control.cpp');
  const power=read('firmware/shared/power.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(runtime,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(detect,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(detect,/odysseySdReleaseLocked\(\);[\s\S]*odysseySdRearmProtocolLocked\("power-transition"\)/);
  assert.match(ota,/otaSession\.state==Synap::COMMITTED[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(ble,/CMD_RESTART:[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(power,/entering deep sleep request=[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*esp_deep_sleep_start\(\)/);
});
