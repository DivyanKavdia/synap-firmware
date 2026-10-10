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

test('C3 mounts readable SD without low-voltage FAT probe writes and still exposes catalogue',()=>{
 assert.match(boot,/sampleBattery\(true\);\s*odysseyInitializeSdCardBeforeBle\(\)/);
 const validate=sd.split('static bool odysseySdValidateVfsLocked(')[1].split('static uint8_t odysseySdMountReasonCode')[0];
 assert.match(validate,/if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) \{[\s\S]*?odysseySdVfsStep=11;[\s\S]*?return true;/);
 assert(validate.indexOf('odysseySdVfsStep=11;')<validate.indexOf('const char* probePath='));
 assert.match(validate,/!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\) \|\|\s*mkdir/);
 assert.match(validate,/opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
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

test('offline write admission has two fresh readings; running recording stops at low voltage',()=>{
 const preflight=rec.split('static bool odysseySdPreflightWritePower() {')[1].split('static void odysseyRecordTake()')[0];
 assert.equal((preflight.match(/sampleBattery\(true\)/g)||[]).length,2);
 assert.match(preflight,/vTaskDelay\(pdMS_TO_TICKS\(80\)\)/);
 assert.match(preflight,/return first && second;/);
 const recordTask=rec.split('static void odysseyRecordTask(void\*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert(recordTask.indexOf('odysseySdPreflightWritePower()')<recordTask.indexOf('odysseyRecoverSdCard("touch")'));
 assert.match(rec,/if \(uint32_t\(millis\(\)-lastPowerSampleAt\)>=5000u\)/);
 assert.match(rec,/if \(weakPowerSamples>=2\) \{[\s\S]*?odysseyStopRequested=true;/);
 assert.match(rec,/odysseyCheckpointWav\(file,checkpointErrno\)/);
});

test('held-busy CMD24/CMD25 failure does not trigger automatic remount',()=>{
 const task=rec.split('static void odysseyRecordTask(void\*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert.match(task,/driverCommand==24u && driverPhase==4u/);
 assert.match(task,/driverCommand==25u && \(driverPhase==4u \|\| driverPhase==6u \|\| driverPhase==7u\)/);
 assert.match(task,/!stuckBusyWrite &&\s*odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)/);
 assert.match(task,/odysseySdUnsafeToSleep=true;/);
 assert.doesNotMatch(task,/SD\.format\(|remove\(|unlink\(/);
});
