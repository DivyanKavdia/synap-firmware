'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {execFileSync}=require('node:child_process'),{createHash}=require('node:crypto');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('C3 generated firmware validates double taps and four-second sleep/wake holds',()=>{
  const c3=materialize(source,'esp32c3-supermini-4m');
  const wake=c3.slice(c3.indexOf('bool confirmTouchWakeTripleTap() {'),c3.indexOf('void publishPowerEvent('));
  const poll=c3.slice(c3.indexOf('void pollTouchControl() {'),c3.indexOf('void updateStatusCharacteristic(bool notify) {'));
  const fixture=fs.readFileSync(path.join(__dirname,'c3-touch.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT WAKE',wake).replace('// INSERT POLL',poll)),/PASS C3/);
});
test('S3 gesture code stays identical to the released triple-tap implementation',()=>{
  assert.equal(materialize(source,'esp32s3-fh4r2-qspi-4m'),source);
  const sections=(source.slice(source.indexOf('bool confirmTouchWakeTripleTap() {'),source.indexOf('void publishPowerEvent('))+
    source.slice(source.indexOf('void pollTouchControl() {'),source.indexOf('void updateStatusCharacteristic(bool notify) {'))).replaceAll('confirmTouchWakeTripleTap','confirmTouchWakeGesture');
  const preprocessed=execFileSync('g++',['-E','-P','-x','c++','-DCONFIG_IDF_TARGET_ESP32S3=1','-DCONFIG_IDF_TARGET_ESP32C3=0','-'],{input:sections,encoding:'utf8'});
  assert.equal(createHash('sha256').update(preprocessed).digest('hex'),'9d797e21bfe546c6079877ecad84f1346ee45ec4e6aec92e0fd7e543eed3769d');
  const c3=materialize(source,'esp32c3-supermini-4m');
  assert.doesNotMatch(c3,/triple tap|pendingDoubleAt|touch-triple/);
  assert.match(c3,/C3_WAKE_HOLD_MS = 4000/);
  assert.match(c3,/C3_SLEEP_HOLD_MS = 4000/);
});
