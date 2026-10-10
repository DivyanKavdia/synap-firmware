'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

const read=name=>fs.readFileSync('firmware/shared/'+name,'utf8');
const recording=read('odyssey-sd-1631-recording.cpp');
const detect=read('odyssey-sd-1631-detect.cpp');

test('stuck-low detection recognizes deselected MISO held low, not normal card idle',()=>{
  const start=detect.indexOf('bool odysseySdBusStuckLow() {');
  const end=detect.indexOf('\nuint16_t odysseySdRawFFCount()',start);
  assert(start>=0 && end>start);
  const source=detect.slice(start,end);
  const code=[
    '#include <atomic>',
    '#include <cstdint>',
    '#include <cassert>',
    '#include <iostream>',
    'std::atomic<uint8_t> odysseySdBootState{2};',
    'std::atomic<int16_t> odysseySdBitBangCsHigh{0};',
    'std::atomic<uint16_t> odysseySdRawZero{1023};',
    source,
    'int main(){',
    'assert(odysseySdBusStuckLow());',
    'odysseySdBitBangCsHigh=1;assert(!odysseySdBusStuckLow());',
    'odysseySdBitBangCsHigh=0;odysseySdRawZero=899;assert(!odysseySdBusStuckLow());',
    'odysseySdRawZero=1024;odysseySdBootState=1;assert(!odysseySdBusStuckLow());',
    'std::cout<<"PASS C3 SD stuck low detector\\n";',
    '}'
  ].join('\n');
  assert.match(nativeTest(code),/PASS C3 SD stuck low detector/);
});

test('C3 offline touch aborts safely before remount when MISO is stuck low',()=>{
  const task=recording.split('static void odysseyRecordTask(void*) {')[1]
    .split('bool odysseyPrepareForConnectedStreaming')[0];
  assert(task);
  const stuck=task.indexOf('if (!odysseySdReady() && odysseySdBusStuckLow())');
  const recover=task.indexOf('odysseyRecoverSdCard("touch")');
  const record=task.indexOf('odysseyRecordTake();');
  // Stuck-low SD must veto recovery before any WAV capture or file creation.
  assert(stuck>=0 && recover>stuck && record>recover);
  assert.doesNotMatch(task,/odysseySdPreflightWritePower/);
  const veto=task.slice(stuck,recover);
  for(const fragment of [
    'odysseyRecording=false;',
    'odysseyStopRequested=false;',
    'applyCpuPowerProfile(false);',
    'odysseyRecordFaultAt=finalizedAt;',
    'updateStatusLed(true);',
    'vTaskDelete(nullptr);',
    'return;'
  ]) assert(veto.includes(fragment),fragment);
  assert(!veto.includes('odysseyRecoverSdCard('),'must not remount during stuck-low veto');
});

test('driver busy-write fault still suppresses auto-rearm without formatting or deleting user WAVs',()=>{
  const task=recording.split('static void odysseyRecordTask(void*) {')[1]
    .split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.match(task,/driverCommand==24u && driverPhase==4u/);
  assert.match(task,/driverCommand==25u && \(driverPhase==4u \|\| driverPhase==6u \|\| driverPhase==7u\)/);
  assert.match(task,/odysseyLastRecordFailureStage\(\) && !stuckBusyWrite/);
  assert.match(task,/odysseySdUnsafeToSleep=true;/);
  assert.doesNotMatch(task,/SD\.format\(|ftruncate\(|unlink\(/);
});
