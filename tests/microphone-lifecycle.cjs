'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('microphone shutdown waits for driver ownership during capture and recovery',()=>{
  const guard=source.slice(source.indexOf('class MicrophoneGuard {'),source.indexOf('#else\n#include <math.h>'));
  const functions=source.slice(source.indexOf('bool startMicrophone() {'),source.indexOf('uint8_t batteryPercentFromMillivolts'));
  const capture=source.slice(source.indexOf('#ifndef SYNAP_MIC_HPF_ENABLE'),source.indexOf('void acquisitionTask(void* parameter) {'));
  const fixture=fs.readFileSync(path.join(__dirname,'microphone-lifecycle.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT GUARD',guard).replace('// INSERT MICROPHONE FUNCTIONS',functions).replace('// INSERT CAPTURE',capture),['-pthread']),/PASS microphone shutdown/);
});
test('both boot and runtime use the same statically allocated microphone lock',()=>{
  assert.match(source,/xSemaphoreCreateRecursiveMutexStatic\(&microphoneMutexStorage\)/);
  assert(source.indexOf('microphoneMutex=xSemaphoreCreateRecursiveMutexStatic')<source.indexOf('microphoneValidated=startMicrophone();'));
  const stop=source.slice(source.indexOf('void stopStreaming(ErrorCode reason) {'),source.indexOf('bool configureTransportFromPeerMtu() {'));
  assert.match(stop,/streamingEnabled.store\(false\);[\s\S]*stopMicrophone\(\);/);
  assert.doesNotMatch(stop,/pdMS_TO_TICKS\(90\)/);
  assert.match(stop,/while \(transmitterActive.load\(\)\) vTaskDelay\(1\);/);
});
