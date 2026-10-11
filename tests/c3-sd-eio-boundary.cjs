'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const recording=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const power=fs.readFileSync('firmware/shared/power.cpp','utf8');
const {nativeTest}=require('./support/native.cjs');

test('a 16,340-byte PCM write failure is at the 16 KiB on-disk boundary',()=>{
  const accepted=16340;
  assert.equal(44+accepted,16384);
  assert.equal((44+accepted)%4096,0);
  assert.equal((44+accepted)/512,32);
  // A cluster boundary is only a hypothesis; never label 0x20SS00EE as CMD25.
  assert.match(detect,/0x20SS00EE/);
  assert.match(recording,/0x20SS00EE/);
  assert.doesNotMatch(recording,/driverCommand==24u|driverCommand==25u|stuckBusyWrite/);
});

test('background idle sleep cannot restart a card after a retained write fault',()=>{
  const sleep=power.split('void enterDeepSleep(const char* reason) {')[1]
    .split('const uint32_t initialReleaseAt=millis();')[0];
  assert.match(sleep,/pendingRecordFault=odysseyLastRecordFailureStage\(\)/);
  assert.match(sleep,/!odysseySdReady\(\) &&[\s\S]*pendingRecordFault>=44u && pendingRecordFault!=48u\) return/);
  assert(sleep.indexOf('pendingRecordFault>=44u')<sleep.indexOf('odysseyRecoverSdCard("sleep")'),
    'unresolved write I/O fault must veto background remount');
});

test('selected BUSY-low is separate from deselected stuck-low and vetoes new WAVs',()=>{
  const fn=detect.split('bool odysseySdControllerBusyLow() {')[1]
    .split('uint16_t odysseySdRawFFCount()')[0];
  assert(fn);
  const code=[
    '#include <atomic>',
    '#include <cstdint>',
    '#include <cassert>',
    '#include <iostream>',
    'std::atomic<uint8_t> odysseySdBootState{2};',
    'std::atomic<int16_t> odysseySdBitBangCsHigh{1};',
    'std::atomic<int16_t> odysseySdBitBangCsLow{1};',
    'std::atomic<uint16_t> odysseySdRawZero{1023};',
    'std::atomic<uint16_t> odysseySdRawFF{0};',
    'bool odysseySdControllerBusyLow() {',fn,
    'int main(){',
    'assert(odysseySdControllerBusyLow());',
    'odysseySdBitBangCsHigh=0;assert(!odysseySdControllerBusyLow());',
    'odysseySdBitBangCsHigh=1;odysseySdRawFF=64;assert(!odysseySdControllerBusyLow());',
    'odysseySdRawFF=0;odysseySdRawZero=10;assert(!odysseySdControllerBusyLow());',
    'odysseySdRawZero=1023;odysseySdBootState=1;assert(!odysseySdControllerBusyLow());',
    'std::cout<<"PASS selected busy-low veto\\n";',
    '}'
  ].join('\n');
  assert.match(nativeTest(code),/PASS selected busy-low veto/);
  const task=recording.split('static void odysseyRecordTask(void*) {')[1]
    .split('bool odysseyPrepareForConnectedStreaming')[0];
  const veto=task.split('if (!odysseySdReady() && odysseySdControllerBusyLow()) {')[1]
    .split('if (!odysseySdReady()) {')[0];
  assert.match(veto,/odysseyRecording=false/);
  assert.match(veto,/vTaskDelete\(nullptr\)/);
  assert.doesNotMatch(veto,/odysseyRecoverSdCard|fopen\(|fwrite\(/);
  const recovery=detect.split('bool odysseyRecoverSdCard(const char* reason) {')[1]
    .split('bool odysseyPrepareSdForPowerTransition')[0];
  assert(recovery.indexOf('odysseySdControllerBusyLow()')<recovery.indexOf('odysseySdReleaseLocked()'));
});

test('native FatFs error never triggers an automatic CMD12, SD re-arm, or deletion',()=>{
  const task=recording.split('static void odysseyRecordTask(void*) {')[1]
    .split('bool odysseyPrepareForConnectedStreaming')[0];
  const afterTake=task.split('odysseyRecordTake();')[1];
  assert.match(afterTake,/skipping automatic remount/);
  assert.match(afterTake,/odysseySdUnsafeToSleep=true/);
  assert.doesNotMatch(afterTake,/odysseyRecoverSdCard\(|odysseySdBitBangRecoverLocked\(|unlink\(|ftruncate\(|\.format\(/);
  // Recovery before capture is still only explicitly user initiated.
  assert.match(task,/odysseyRecoverSdCard\("touch"\)/);
});

test('microphone-only and zero-audio failures do not invalidate a healthy SD',()=>{
  const take=recording.split('static void odysseyRecordTake() {')[1]
    .split('static void odysseyRecordTask(void*) {')[0];
  assert.match(take,/const bool storageFailure=persistedStage!=43u && persistedStage!=48u/);
  const segment=take.split('const bool storageFailure=')[1].split('} else {\n    odysseyPersistRecordFailure(0,0);')[0];
  assert.match(segment,/if \(storageFailure\)/);
  assert.match(segment,/odysseySdBootState=2/);
  assert.doesNotMatch(segment,/remove\(|unlink\(|format/);
});

test('existing FAT volume uses lower-speed SDSPI and status checks without formatting',()=>{
  assert.match(detect,/ODYSSEY_SD_DATA_FREQ_HZ=800000u/);
  assert.match(detect,/host.max_freq_khz=ODYSSEY_SD_DATA_FREQ_HZ\/1000u/);
  assert.match(detect,/mount.disk_status_check_enable=true/);
  assert.match(detect,/mount.format_if_mount_failed=false/);
  assert.doesNotMatch(detect,/esp_vfs_fat_sdcard_format\(/);
  assert.match(detect,/ODYSSEY_SD_MAX_OPEN_FILES=1/);
});

test('read-only first-fault diagnostics include exact offset and correct VFS label',()=>{
  const op=transfer.split('case 27: {')[1].split('case 4:')[0];
  assert.match(op,/odysseyLastRecordFailureBytes\(\)\+44u/);
  assert.match(op,/failureOffset/);
  assert.match(op,/sdClockKhz/);
  assert.match(op,/faultKind/);
  assert.doesNotMatch(op,/fopen\(|fwrite\(|unlink\(|odysseyRecoverSdCard\(/);
});
