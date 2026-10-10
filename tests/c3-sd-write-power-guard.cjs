'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const read=p=>fs.readFileSync(p,'utf8');
const runtime=read('firmware/shared/runtime.cpp'),boot=read('firmware/shared/boot.cpp');
const detect=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');

test('Rev K battery write policy rejects untrusted/low voltage with hysteresis',()=>{
 const a=runtime.indexOf('constexpr uint16_t ODYSSEY_SD_WRITE_START_MIN_MV=');
 const b=runtime.indexOf('void odysseyToggleRecording();',a);
 assert(a>=0&&b>a);
 const gate=runtime.slice(a,b);
 const cpp=[
 '#include <atomic>','#include <cstdint>','#include <cassert>','#include <iostream>',
 'std::atomic<bool> batteryAvailable{false};','uint16_t batteryMillivolts=0;',
 gate,
 'int main(){',
 'assert(ODYSSEY_SD_WRITE_START_MIN_MV>ODYSSEY_SD_WRITE_CONTINUE_MIN_MV);',
 'for(unsigned cell:{0u,2800u,3400u,3508u,3799u,3800u,3899u,3900u,4150u,4350u,4351u}){',
 'batteryMillivolts=cell;batteryAvailable=false;',
 'assert(!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV));',
 'batteryAvailable=true;',
 'assert(odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV)==(cell>=3900u&&cell<=4350u));',
 'assert(odysseySdPowerSafe(ODYSSEY_SD_WRITE_CONTINUE_MIN_MV)==(cell>=3800u&&cell<=4350u));',
 '} std::cout<<"PASS SD voltage policy\\n";}'
 ].join('\n');
 assert.match(nativeTest(cpp),/PASS SD voltage policy/);
});

test('boot does not run a FAT write probe with untrusted SD supply',()=>{
 const bootC3=boot.split('OdysseyWifi::initialize();')[1].split('initializeBLE();')[0];
 assert.match(bootC3,/OdysseyTransfer::initialize\(\);[\s\S]*?sampleBattery\(true\);\s*odysseyInitializeSdCardBeforeBle\(\)/);
 const validation=detect.split('static bool odysseySdValidateVfsLocked(')[1].split('static uint8_t odysseySdMountReasonCode')[0];
 const low=validation.indexOf('if (!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV))');
 const write=validation.indexOf('FILE* probe=fopen(probePath,"wb")');
 assert(low>=0&&write>low);
 assert.match(validation,/odysseySdVfsStep=11;odysseySdVfsErrno=0/);
 assert.match(validation,/!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\) \|\|[\s\S]*?mkdir\(ODYSSEY_SD_RECORDING_DIR,0755\)/);
 assert.doesNotMatch(validation.slice(low,write),/fputc|unlink\(probePath\)/);
});

test('offline recording refuses weak start and stops gracefully as voltage drops',()=>{
 const preflight=recorder.split('static bool odysseySdPreflightWritePower() {')[1].split('static void odysseyRecordTake()')[0];
 assert.equal((preflight.match(/sampleBattery\(true\);/g)||[]).length,2);
 assert.match(preflight,/return first && second;/);
 const worker=recorder.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert(worker.indexOf('if (!odysseySdPreflightWritePower())')<worker.indexOf('odysseyRecoverSdCard("touch")'));
 const take=recorder.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*)')[0];
 assert.match(take,/weakPowerSamples>=2/);
 assert.match(take,/odysseyStopRequested=true;/);
 assert.match(take,/odysseyCheckpointWav\(file,checkpointErrno\)/);
 assert.match(take,/fclose\(file\)!=0/);
});

test('busy-write timeout suppresses automatic rearm and read-only sync remains accessible',()=>{
 const worker=recorder.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
 assert.match(worker,/driverCommand==24u && driverPhase==4u/);
 assert.match(worker,/driverCommand==25u && \(driverPhase==4u \|\| driverPhase==6u \|\| driverPhase==7u\)/);
 assert.match(worker,/!stuckBusyWrite &&[\s\S]*?odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)/);
 assert.match(worker,/odysseySdUnsafeToSleep=true;/);
 const readSelected=transfer.split('static uint8_t readSelected(')[1].split('static uint8_t catalogue(')[0];
 assert.doesNotMatch(readSelected,/odysseySdPowerSafe|batteryCritical/);
 const remove=transfer.split('static uint8_t removeFile(')[1].split('static uint16_t clearRecordings()')[0];
 assert.match(remove,/if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) return BUSY;/);
 assert.match(transfer,/else if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) error=BUSY;/);
 assert.doesNotMatch(recorder,/SD\.format\(|ftruncate\(/);
});
