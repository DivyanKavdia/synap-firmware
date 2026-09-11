'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('BLE link changes survive queue pressure and stale commands cannot starve maintenance',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const types=source.slice(source.indexOf('enum class DeviceState'),source.indexOf('struct AudioFrame'));
  const start=source.slice(source.indexOf('void startStreaming(uint8_t version) {'),source.indexOf('void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream) {'));
  const callbacks=source.slice(source.indexOf('class ServerCallbacks'),source.indexOf('class ControlCallbacks'));
  const task=source.slice(source.indexOf('void reconcileConnection() {'),source.indexOf('#ifndef SYNAP_MIC_HPF_ENABLE'));
  const fixture=fs.readFileSync(path.join(__dirname,'control-runtime.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT CONTROL TYPES',types).replace('// INSERT START',start).replace('// INSERT CALLBACKS',callbacks).replace('// INSERT CONTROL TASK',task)),/PASS link recovery/);
});
