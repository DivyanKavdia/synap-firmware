'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

const read=(name)=>fs.readFileSync('firmware/shared/'+name,'utf8');
const runtime=read('runtime.cpp');
const recorder=read('odyssey-sd-1631-recording.cpp');
const detect=read('odyssey-sd-1631-detect.cpp');
const transfer=read('odyssey-sd-1631-transfer.cpp');
const boot=read('boot.cpp');

test('C3 Rev K battery admission has correct 3.9V / 3.8V hysteresis and rejects invalid ADC',()=>{
  const start=runtime.indexOf('constexpr uint16_t ODYSSEY_SD_WRITE_START_MIN_MV');
  const end=runtime.indexOf('void odysseyToggleRecording();',start);
  assert(start>=0 && end>start);
  const policy=runtime.slice(start,end);
  const before=[
    '#include <atomic>',
    '#include <cstdint>',
    '#include <cassert>',
    '#include <iostream>',
    'std::atomic<bool> batteryAvailable{false};',
    'uint16_t batteryMillivolts=0;'
  ].join('\n');
  const after=[
    'int main() {',
    'batteryMillivolts=4200; assert(!odysseySdPowerSafe(3900));',
    'batteryAvailable=true;',
    'batteryMillivolts=3492; assert(!odysseySdPowerSafe(3800));',
    'batteryMillivolts=3799; assert(!odysseySdPowerSafe(3800));',
    'batteryMillivolts=3800; assert(odysseySdPowerSafe(3800)); assert(!odysseySdPowerSafe(3900));',
    'batteryMillivolts=3899; assert(!odysseySdPowerSafe(3900));',
    'batteryMillivolts=3900; assert(odysseySdPowerSafe(3900));',
    'batteryMillivolts=4200; assert(odysseySdPowerSafe(3900));',
    'batteryMillivolts=4351; assert(!odysseySdPowerSafe(3900));',
    'batteryAvailable=false; assert(!odysseySdPowerSafe(3900));',
    'std::cout<<"PASS C3 SD battery admission thresholds\\n";',
    '}'
  ].join('\n');
  assert.match(nativeTest(before+'\n'+policy+'\n'+after),/PASS C3 SD battery admission thresholds/);
});

test('C3 boot samples calibrated Rev K battery before any SD mount can write',()=>{
  const setup=boot.split('void setup() {')[1].split('void loop() {')[0];
  assert(setup);
  const init=setup.indexOf('odysseyInitializeSdCardBeforeBle();');
  const start=setup.indexOf('OdysseyTransfer::initialize();');
  const sample=setup.indexOf('sampleBattery(true);',start);
  assert(start>=0 && sample>start && init>sample,'must sample AFTER initialization, BEFORE SD probe');
  assert.match(boot,/analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_11db\)/);
  assert.match(boot,/Rev K C3 uses a 470 kOhm \/ 470 kOhm battery divider/);
});

test('low-battery SD mount can enumerate existing recordings without running FAT write probe',()=>{
  const fn=detect.split('static bool odysseySdValidateVfsLocked(')[1].split('static bool odysseySdMountOnceLocked')[0];
  assert(fn);
  assert(fn.indexOf('opendir(ODYSSEY_SD_RECORDING_DIR)')<
    fn.indexOf('if (!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV))'));
  assert(fn.indexOf('if (!odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV))')<
    fn.indexOf('fopen(probePath,"wb")'));
  assert.match(fn,/return true;[\s\S]*?const char\* probePath=/);
  assert.match(fn,/odysseySdVfsStep=11;odysseySdVfsErrno=0/);
  assert.doesNotMatch(fn,/SD\.format\(/);
  // Arduino SD.begin is not a VFS read_only mount: only the writable
  // application test is skipped, and deliberate writes remain guarded.
  assert.match(detect,/SD\.begin\(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,/);
});

test('offline recording refuses weak/unknown cell BEFORE mounting and writing and stops if it drops',()=>{
  const task=recorder.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  const capture=recorder.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
  assert(task && capture);
  const preflight=task.indexOf('odysseySdPreflightWritePower()');
  const rearm=task.indexOf('odysseyRecoverSdCard("touch")');
  const recording=task.indexOf('odysseyRecordTake();');
  assert(preflight>=0 && preflight<rearm && rearm<recording);
  assert.match(task,/if \(!odysseySdReady\(\) && odysseySdBusStuckLow\(\)\)/);
  assert(task.indexOf('odysseySdBusStuckLow()')<rearm);
  assert.match(task,/vTaskDelete\(nullptr\);[\s\S]*?return;[\s\S]*?odysseyRecoverSdCard\("touch"\)/);
  const pre=recorder.split('static bool odysseySdPreflightWritePower() {')[1].split('static void odysseyRecordTake')[0];
  assert.equal((pre.match(/sampleBattery\(true\)/g)||[]).length,2);
  assert.match(pre,/vTaskDelay\(pdMS_TO_TICKS\(80\)\)/);
  assert.match(pre,/return first && second;/);
  const posWrite=capture.indexOf('fwrite(batch,1,sizeof(batch),file)');
  const posMonitor=capture.indexOf('if (uint32_t(millis()-lastPowerSampleAt)>=5000u)');
  assert(posWrite>=0 && posMonitor>posWrite);
  assert.match(capture,/weakPowerSamples>=2[\s\S]*?odysseyStopRequested=true/);
  assert.match(capture,/odysseyCheckpointWav\(file,checkpointErrno\)/);
  assert.doesNotMatch(capture,/SD\.format\(|ftruncate\(|fseek\(file,0/);
});

test('SD sync read remains possible at low battery, while delete/clear require admission',()=>{
  const catalogue=transfer.split('static uint8_t catalogue(')[1].split('static uint8_t removeFile(')[0];
  const readSelected=transfer.split('static uint8_t readSelected(')[1].split('static uint8_t catalogue(')[0];
  const remove=transfer.split('static uint8_t removeFile(')[1].split('static uint16_t clearRecordings')[0];
  assert(catalogue && readSelected && remove);
  assert.doesNotMatch(catalogue,/odysseySdPowerSafe\(/);
  assert.doesNotMatch(readSelected,/odysseySdPowerSafe\(/);
  assert.match(remove,/if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) return BUSY/);
  assert.match(transfer,/static uint16_t clearRecordings\(\) \{\s*if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) return 0/);
  assert.match(transfer,/else if \(!odysseySdPowerSafe\(ODYSSEY_SD_WRITE_START_MIN_MV\)\) error=BUSY;/);
  assert.doesNotMatch(transfer,/SD\.format\(/);
});

test('failing SD CMD24/CMD25 busy-stops are not automatically remounted at task completion',()=>{
  assert.match(recorder,/driverCommand==24u && driverPhase==4u/);
  assert.match(recorder,/driverCommand==25u && \(driverPhase==4u \|\| driverPhase==6u \|\| driverPhase==7u\)/);
  assert.match(recorder,/if \(!odysseySdReady\(\) && odysseyLastRecordFailureStage\(\) && !stuckBusyWrite &&/);
  assert.match(recorder,/odysseySdUnsafeToSleep=true/);
});
