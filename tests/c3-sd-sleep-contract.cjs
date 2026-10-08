'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const detect=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
const power=read('firmware/shared/power.cpp');
const ota=read('firmware/shared/ota.cpp');
const control=read('firmware/shared/ble-control.cpp');
const runtime=read('firmware/shared/runtime.cpp');

test('C3 cannot enter deep sleep or reboot on a non-idle SD bus',()=>{
 const prepare=detect.split('bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {')[1].split('// Odyssey S3 remains detection-only')[0];
 assert.match(prepare,/OdysseySdGuard guard\(pdMS_TO_TICKS\(timeoutMs\)\)/);
 assert.match(prepare,/if \(!guard\) \{[\s\S]*?return false;/);
 assert.match(prepare,/if \(odysseyRecording\.load\(\)\) \{[\s\S]*?return false;/);
 assert.match(prepare,/odysseySdQuiesceLocked\(ODYSSEY_SD_QUIESCE_BUDGET_MS\)/);
 assert.match(prepare,/if \(odysseySdUnsafeToSleep\.load\(\)\)/);
 assert.match(prepare,/if \(idle!=1\) \{[\s\S]*?odysseySdUnsafeToSleep=true;[\s\S]*?return false;\s*\}/);
 assert.match(detect,/odysseySdUnsafeToSleep=false;/);
 assert.doesNotMatch(prepare,/quiesced=%u[^\n]*\n\s*return true;/);
 const deep=power.split('void enterDeepSleep(const char* reason) {')[1].split('void powerTick() {')[0];
 assert.match(deep,/if \(odysseyRecording\.load\(\) \|\| OdysseyWifi::busy\(\)\) return;/);
 assert.match(deep,/if \(!odysseyPrepareSdForPowerTransition\(1000u\)\) \{[\s\S]*?sleepPending=false;[\s\S]*?return;[\s\S]*?esp_deep_sleep_start\(\);/);
 const wake=power.split('void armTouchWakeAndSleep() {')[1].split('bool confirmTouchWakeGesture() {')[0];
 assert.equal((wake.match(/if \(!odysseyPrepareSdForPowerTransition\(500u\)\)/g)||[]).length,3);
 assert.match(ota,/if \(!odysseyPrepareSdForPowerTransition\(1000u\)\) return;\s*#endif\s*ESP\.restart\(\)/);
 assert.match(control,/if \(!odysseyPrepareSdForPowerTransition\(1000u\)\) \{/);
});

test('C3 SD recorder keeps sleep veto through final batch, fclose, persistent diagnostics and recovery',()=>{
 const take=recorder.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
 assert.match(take,/fwrite\(batch,1,sizeof\(batch\),file\)/);
 assert.match(take,/if \(fclose\(file\)!=0\)/);
 const worker=recorder.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 const takeEnd=worker.indexOf('odysseyRecordTake();');
 const recoverEnd=worker.indexOf('odysseyRecoverSdCard("rearm")');
 const settle=worker.lastIndexOf('odysseySdSleepGuardUntil=finalizedAt+5000u;');
 const idle=worker.lastIndexOf('odysseyRecording=false;');
 assert(takeEnd>=0 && recoverEnd>takeEnd && settle>recoverEnd && idle>settle);
 assert.match(worker,/disconnectedAt=finalizedAt;/);
 const failedRecovery=worker.split('if (!odysseyRecoverSdCard("touch")) {')[1].split('return;')[0];
 assert(failedRecovery.indexOf('odysseySdSleepGuardUntil=finalizedAt+5000u;') <
   failedRecovery.indexOf('odysseyRecording=false;'));
 assert.match(runtime,/std::atomic<uint32_t> odysseySdSleepGuardUntil\{0\}/);
 assert.match(runtime,/std::atomic<bool> odysseySdUnsafeToSleep\{false\}/);
 assert.match(worker,/if \(!odysseySdReady\(\) && lastFault>=44u && lastFault!=48u\) \{[\s\S]*?odysseySdUnsafeToSleep=true;/);
});

test('all automatic and touch-triggered sleep entries defer until recording finalized and SD settled',()=>{
 const tick=power.split('void powerTick() {')[1].split('void pollTouchControl() {')[0];
 assert.match(tick,/if \(odysseyRecording\.load\(\)\) \{\s*if \(batteryCritical\(\)\) odysseyStopRequested=true;\s*return;/);
 assert.match(tick,/if \(OdysseyWifi::busy\(\)\) return;/);
 assert.match(tick,/odysseySdSleepGuardUntil\.load\(\)/);
 assert.match(tick,/enterDeepSleep\("critical-battery"\)/);
 assert.match(tick,/enterDeepSleep\("disconnected-timeout"\)/);
 const hold=power.split('void pollTouchControl() {')[1];
 assert.match(hold,/if \(deepSleepAfterStop && !streaming && !raw && !otaBusy\(\)[\s\S]*?odysseySdSleepGuardUntil\.load\(\)[\s\S]*?\) \{\s*deepSleepAfterStop=false;\s*enterDeepSleep\("touch-hold-after-stop"\);/);
 assert.match(hold,/if \(odysseyRecording\.load\(\)\) \{\s*odysseyStopRequested=true;\s*deepSleepAfterStop=true;/);
 assert.match(hold,/enterDeepSleep\("touch-hold"\)/);
});

test('C3 SD firmware still retains append-only WAV and non-formatting SPI mount',()=>{
 assert.match(recorder,/alignas\(4\) static uint8_t batch\[4096\]/);
 assert.doesNotMatch(recorder,/fseek\(file,0|ftruncate\(/);
 assert.match(detect,/ODYSSEY_SD_DATA_FREQ_HZ=1000000u/);
 assert.match(detect,/odysseySdBeginLocked\(\)/);
});
