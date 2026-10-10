'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const r=fs.readFileSync('firmware/shared/runtime.cpp','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');

test('Rev K low-voltage gate is conservative and hysteretic, with native thresholds',()=>{
 const start=r.indexOf('constexpr uint16_t ODYSSEY_SD_WRITE_START_MIN_MV=');
 const end=r.indexOf('void odysseyToggleRecording();',start);
 assert(start>0&&end>start);
 const helper=r.slice(start,end);
 const code=`
#include <atomic>
#include <cstdint>
#include <cassert>
#include <iostream>
std::atomic<bool> batteryAvailable{false};
uint16_t batteryMillivolts=0;
${helper}
int main(){
 assert(ODYSSEY_SD_WRITE_START_MIN_MV==3900);
 assert(ODYSSEY_SD_WRITE_CONTINUE_MIN_MV==3800);
 assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));
 batteryMillivolts=4200;
 assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));
 batteryAvailable=true;
 for (auto mv:{uint16_t(2800),uint16_t(3492),uint16_t(3800),uint16_t(3899)})
   {batteryMillivolts=mv; assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));}
 batteryMillivolts=3900;
 assert(odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));
 batteryMillivolts=3850;
 assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));
 assert(odysseySdPowerSafe(ODYSSEY_SD_WRITE_CONTINUE_MIN_MV));
 batteryMillivolts=4400;
 assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));
 std::cout<<"PASS Rev K SD conservative low-voltage policy\\n";
}
`;
 assert.match(nativeTest(code),/PASS Rev K SD conservative low-voltage policy/);
});

test('C3 SD boot mount is ADC-independent and read-only, even without /synap',()=>{
 const validate=sd.split('static bool odysseySdValidateVfsLocked(')[1].split('static uint8_t odysseySdMountReasonCode')[0];
 assert.match(boot,/restoreOdysseySdBatteryDividerProfile\(\);\s*sampleBattery\(true\);\s*odysseyInitializeSdCardBeforeBle\(\)/);
 assert.doesNotMatch(validate,/odysseySdPowerSafe\(|batteryAvailable|batteryMillivolts/);
 assert.doesNotMatch(validate,/mkdir\(|fopen\(|unlink\(|remove\(|fflush\(|fsync\(|fputc\(/);
 assert.match(validate,/if \(directoryErrno==ENOENT\)/);
 assert.match(validate,/odysseySdVfsStep=13;odysseySdVfsErrno=ENOENT;/);
 assert.match(validate,/opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
 assert.match(validate,/closedir\(verified\)/);
 assert.match(validate,/odysseySdVfsStep=11;odysseySdVfsErrno=0/);
 assert.match(sd,/odysseySdBootState=1;\s*odysseySdProbeStage=6/);
 assert.match(transfer,/case 7:\s*error=catalogue\(total\)/);
 assert.match(transfer,/FILE\* file=fopen\(full,"rb"\)/);
});

test('destructive media operations require safe supply; user never sees successful deletion on reject',()=>{
 assert.match(transfer,/static uint8_t removeFile[\s\S]*?if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) return BUSY;/);
 assert.match(transfer,/static uint16_t clearRecordings[\s\S]*?if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) return 0;/);
 assert.match(transfer,/case 18:[\s\S]*?if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) error=BUSY/);
 assert.doesNotMatch(transfer,/SD\.format\(/);
});

test('offline gesture reaches real C3 SD writes and verifies WAV after fclose',()=>{
 const task=rec.split('static void odysseyRecordTask(void*) {')[1]
    .split('bool odysseyPrepareForConnectedStreaming')[0];
 const take=rec.split('static void odysseyRecordTake() {')[1]
    .split('static void odysseyRecordTask(void*) {')[0];
 assert.doesNotMatch(task,/odysseySdPreflightWritePower/);
 assert.match(task,/odysseySdBusStuckLow/);
 assert.match(task,/odysseyRecoverSdCard\("touch"\)/);
 assert(task.indexOf('odysseyRecordTake();')>task.indexOf('odysseyRecoverSdCard("touch")'));
 assert.match(take,/OdysseySdGuard storage;/);
 assert.match(take,/mkdir\(ODYSSEY_SD_RECORDING_DIR,0755\)/);
 assert.doesNotMatch(take,/odysseySdPowerSafe\(/);
 assert.match(take,/file=fopen\(fullPath,"wb"\)/);
 assert.match(take,/fwrite\(batch,1,sizeof\(batch\),file\)/);
 assert.match(take,/odysseyCheckpointWav\(file,checkpointErrno\)/);
 assert.match(take,/fclose\(file\)/);
 assert.match(take,/stat\(fullPath,&persisted\)/);
 assert.match(take,/fopen\(fullPath,"rb"\)/);
 assert.match(take,/memcmp\(headerReadback,"RIFF",4\)/);
 assert.match(take,/failureStage=74/);
 assert.match(take,/failureStage=75/);
 assert.match(take,/WAV VERIFIED/);
 assert.match(rec,/deviceConnected.load\(\) \|\| streamingEnabled.load\(\)/);
 assert.match(rec,/xTaskCreate\(odysseyRecordTask/);
});

test('held-busy CMD24/CMD25 failure does not trigger automatic remount',()=>{
 const task=rec.split('static void odysseyRecordTask(void\*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert.match(task,/driverCommand==24u && driverPhase==4u/);
 assert.match(task,/driverCommand==25u && \(driverPhase==4u \|\| driverPhase==6u \|\| driverPhase==7u\)/);
 assert.match(task,/!stuckBusyWrite\) \{/);
 assert.match(task,/odysseySdUnsafeToSleep=true;/);
 assert.doesNotMatch(task,/SD\.format\(|remove\(|unlink\(/);
});

test('C3 accepts a freshly formatted readable FAT root with missing /synap and stale recorder NVS',()=>{
 const validate=sd.split('static bool odysseySdValidateVfsLocked(')[1]
   .split('static uint8_t odysseySdMountReasonCode')[0];
 const missing=validate.split('if (directoryErrno==ENOENT)')[1];
 assert(missing);
 assert.match(missing,/odysseySdVfsStep=13;odysseySdVfsErrno=ENOENT;/);
 assert.match(missing,/return true;/);
 assert.doesNotMatch(validate,/mkdir\(ODYSSEY_SD_RECORDING_DIR/);
 const list=transfer.split('static uint8_t catalogue(uint32_t& total) {')[1]
   .split('static uint8_t removeFile(')[0];
 assert.match(list,/if \(directoryErrno==ENOENT\)/);
 assert.match(list,/stat\(odysseySdMountPoint\(\),&root\)==0 && S_ISDIR\(root.st_mode\)/);
 assert.match(list,/catalogueBuffer="\[\]";\s*total=2u;[\s\S]*?return OK;/);
 assert.match(list,/catalogueErrno=directoryErrno;[\s\S]*?return IO_ERROR;/);
 const capture=rec.split('static void odysseyRecordTake() {')[1]
   .split('static void odysseyRecordTask(void*) {')[0];
 assert.match(capture,/stat\(ODYSSEY_SD_RECORDING_DIR,&recordingDir\)/);
 assert.match(capture,/mkdir\(ODYSSEY_SD_RECORDING_DIR,0755\)/);
 assert(capture.indexOf('mkdir(ODYSSEY_SD_RECORDING_DIR,0755)')<
        capture.indexOf('file=fopen(fullPath,"wb")'));
 const task=rec.split('static void odysseyRecordTask(void*) {')[1];
 assert(task.indexOf('odysseySdBusStuckLow()')<task.indexOf('odysseyRecordTake()'));
});

test('power-loss boot policy: fresh C3 POWERON tries mount before raw SD protocol re-arm',()=>{
 const bootFn=sd.split('bool odysseyInitializeSdCardBeforeBle() {')[1].split('bool odysseyRecoverSdCard(')[0];
 assert.match(bootFn,/previousStorageFault/);
 assert.match(bootFn,/bootResetReason!=ESP_RST_POWERON/);
 assert.match(bootFn,/odysseySdBitBangRecoverLocked\("rearm"\)/);
 assert.match(bootFn,/odysseySdMountLocked\("boot",ODYSSEY_SD_BOOT_ATTEMPTS\)/);
 assert(bootFn.indexOf('bootResetReason!=ESP_RST_POWERON')<bootFn.indexOf('odysseySdBitBangRecoverLocked("rearm")'));
 assert.match(sd,/if \(!mounted\) \{[\s\S]*?odysseySdBitBangRecoverLocked\(reason\)/);
 assert.doesNotMatch(bootFn,/SD\.format\(|formatIfMountFailed/);
});
