'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..'),read=p=>fs.readFileSync(path.join(root,p),'utf8');
test('C3 SD mutex is released before a recording task deletes itself',()=>{
  const source=read('firmware/shared/odyssey-sd-recording.cpp');
  const take=source.split('static bool odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
  const task=source.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.match(take,/OdysseySdGuard storage;/);
  assert.doesNotMatch(take,/vTaskDelete/);
  assert.match(task,/const bool storageFault=odysseyRecordTake\(\);[\s\S]*odysseyRecording=false;[\s\S]*vTaskDelete\(nullptr\)/);
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
  assert.match(detect,/ODYSSEY_SD_BOOT_ATTEMPTS=1/);
  assert.match(detect,/ODYSSEY_SD_RECOVERY_ATTEMPTS=1/);
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
  assert.match(recorder,/if \(failed \|\| bytes==0 \|\| finalSize<=long\(sizeof\(header\)\)\) odysseyRecordFaultAt=millis\(\)/);
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
test('C3 catalogue failure quiesces the mounted session without auto-remounting',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(transfer,/case 7:[\s\S]*?error=catalogue\(total\)/);
  const catalogueCase=transfer.split('case 7:')[1].split('case 8:')[0];
  assert.doesNotMatch(catalogueCase,/odysseyRecoverSdCard\(/);
  assert.match(catalogueCase,/odysseySdMarkVfsFailure\(\)/);
  assert.match(catalogueCase,/odysseySdQuiesceFaultedSession\(750u\)/);
  assert.match(transfer,/case 14:[\s\S]*odysseyRecoverSdCard\("op14"\)/);
  assert.match(transfer,/sdProbe/);
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


test('C3 GPIO bitbang fallback bypasses SPIClass only after exact mount failure',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  assert.match(detect,/odysseySdBitBangTransfer/);
  assert.match(detect,/pinMode\(ODYSSEY_SD_MISO,INPUT_PULLUP\)/);
  assert.match(detect,/odysseySdBitBangTransfer\(0xFD\)/);
  assert.match(detect,/odysseySdBitBangCmd12Ready/);
  assert.match(detect,/REQUIRED_IDLE_BYTES=64u/);
  assert.match(detect,/MAX_DRAIN_BYTES=8192u/);
  assert.match(detect,/odysseySdBitBangStopReadLocked\(cmd12,drainBytes\)/);
  assert.match(detect,/odysseySdBitBangCommand\(0u,0u,0x95u\)/);
  assert.match(detect,/odysseySdBitBangCommand\(8u,0x1AAu,0x87u/);
  assert.match(detect,/digitalRead\(ODYSSEY_SD_MISO\)/);
  assert(detect.indexOf('bool mounted=odysseySdBeginLocked();')<detect.indexOf('odysseySdBitBangRecoverLocked(reason)'));
  assert.match(detect,/if \(bitBangCmd0==0x01\)[\s\S]*mounted=odysseySdBeginLocked\(\)/);
  assert.doesNotMatch(detect,/odysseySdRearmProtocolLocked|ODYSSEY_SD_RESCUE_FREQ_HZ/);
  assert.match(transfer,/bbHigh/);
  assert.match(transfer,/bbLow/);
  assert.match(transfer,/raw0/);
  assert.match(transfer,/rawFF/);
  assert.match(transfer,/rawFE/);
  assert.match(transfer,/rawOther/);
  assert.match(transfer,/rawMaxFF/);
  assert.match(detect,/odysseySdSampleRawLocked/);
  assert.match(detect,/for \(uint16_t i=0;i<1024u;\+\+i\)/);
  assert.match(transfer,/mountWhy/);
  assert.match(transfer,/bbStop/);
  assert.match(transfer,/bbCmd12Candidate/);
  assert.match(transfer,/bbReadIdle/);
  assert.match(transfer,/bbDrain/);
  assert.match(transfer,/bbCmd0/);
  assert.match(transfer,/bbCmd8/);
  assert.match(transfer,/bbR7/);
});
test('C3 quiesces SD before OTA reboot, app restart and deep sleep',()=>{
  const runtime=read('firmware/shared/runtime.cpp');
  const ota=read('firmware/shared/ota.cpp');
  const ble=read('firmware/shared/ble-control.cpp');
  const power=read('firmware/shared/power.cpp');
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  assert.match(runtime,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(detect,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  const transition=detect.split('bool odysseyPrepareSdForPowerTransition')[1];
  assert.match(transition,/odysseySdReleaseLocked\(\)/);
  // Quiesce means CMD12 and a drain to idle, not just SD.end(). SD.end() tears
  // down the host and sends the card nothing, which is how a reset mid-CMD18
  // left the next boot facing a bus stuck at 0x00.
  assert.match(transition,/odysseySdQuiesceLocked\(ODYSSEY_SD_QUIESCE_BUDGET_MS\)/);
  // Runtime readiness may fall after EIO; mounted-session ownership survives.
  assert.match(detect,/odysseySdMountedSession\{false\}/);
  assert.match(detect,/odysseySdMountedSession=true;[\s\S]*odysseySdValidateVfsLocked/);
  assert.match(transition,/odysseySdMountedSession\.exchange\(false\)/);
  assert.match(transition,/if \(!hadMountedSession\)[\s\S]*without quiesce/);
  assert.doesNotMatch(transition,/const bool wasReady=odysseySdReady\(\)/);
  // Full re-detection must not run on the way out of the process.
  assert.doesNotMatch(transition,/odysseySdBitBangRecoverLocked|odysseySdMountLocked/);
  const quiesce=detect.split('static uint8_t odysseySdQuiesceLocked')[1].split('\n}')[0];
  assert.match(quiesce,/odysseySdBitBangStopReadLocked\(candidate,drained,budgetMs\)/);
  assert.match(quiesce,/odysseySdBitBangStopWriteLocked\(/,'CMD25 writers also need stopping');
  assert.match(quiesce,/digitalWrite\(ODYSSEY_SD_CS,HIGH\)/);
  // The drain must be able to give up: a reset path cannot block on a card
  // that will never answer.
  assert.match(detect,/if \(budgetMs && uint32_t\(millis\(\)-deadlineStarted\)>=budgetMs\) break;/);
  assert.match(ota,/otaSession\.state==Synap::COMMITTED[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(ble,/CMD_RESTART:[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*ESP\.restart\(\)/);
  assert.match(power,/entering deep sleep request=[\s\S]*odysseyPrepareSdForPowerTransition\(1000u\)[\s\S]*esp_deep_sleep_start\(\)/);
});

test('C3 offline storage faults are marked, cleaned up after guard release and rearmed once',()=>{
  const recording=read('firmware/shared/odyssey-sd-recording.cpp');
  assert.match(recording,/static bool odysseyRecordTake\(\)/);
  assert.match(recording,/fwrite\(header,1,sizeof\(header\),file\).*fflush\(file\)/s);
  assert.match(recording,/written!=sizeof\(pcm\)[\s\S]*odysseySdMarkVfsFailure\(\)/);
  assert.match(recording,/odysseyCheckpointWav\(file,header,bytes\)[\s\S]*odysseySdMarkVfsFailure\(\)/);
  assert.match(recording,/fclose\(file\)!=0[\s\S]*odysseySdMarkVfsFailure\(\)/);
  assert.match(recording,/file bytes=%ld/);
  const task=recording.split('static void odysseyRecordTask')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.match(task,/const bool storageFault=odysseyRecordTake\(\)/);
  assert(task.indexOf('odysseyRecording=false;')<task.indexOf('odysseySdQuiesceFaultedSession(750u)'));
  assert.match(task,/odysseySdQuiesceFaultedSession\(750u\)/);
  assert.match(task,/odysseySdRequestRecovery\(\)/);
});

test('C3 media v2 reuses the proven eight-credit notification window with v1 fallback',()=>{
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const caps=read('firmware/shared/module-capabilities.cpp');
  assert.match(transfer,/4fa1235a-0000-1000-8000-00805f9b34fb/);
  assert.match(transfer,/case 12: streamWindow\(request\); continue;/);
  assert.match(transfer,/request\.operation==16[\s\S]*\+\+cancelWindow/);
  assert.match(transfer,/count<8/);
  assert.match(transfer,/fopen\(full,"rb"\)[\s\S]*for \(uint8_t count=0;!error && count<8/);
  assert.match(transfer,/sendMediaPacket\(request,1,OK,total,offset,bytes,size\)/);
  assert.match(transfer,/endMediaWindow\(request,error,total,offset\)/);
  assert.match(transfer,/case 4:/,'media-v1 read must remain as fallback');
  assert.match(caps,/p\[14\]=1/);
  assert.match(caps,/p\[16\]=OdysseyTransfer::streamAvailable\(\)\?1:0/);
});
