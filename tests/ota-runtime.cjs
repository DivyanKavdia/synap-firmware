'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('OTA capability refresh and rejected BEGIN preserve an active recording',()=>{
  const engine=source.split('// BEGIN EMBEDDED OtaSession.h')[1].split('// END EMBEDDED OtaSession.h')[0];
  const publish=source.slice(source.indexOf('void otaPublish(bool notify) {'),source.indexOf('void otaInitialize(BLEService* service) {'));
  const tick=source.slice(source.indexOf('void otaTick() {'),source.indexOf('// END EMBEDDED SynapOTA.h'));
  const fixture=fs.readFileSync(path.join(__dirname,'ota-runtime.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT ENGINE',engine).replace('// INSERT PUBLISH',publish).replace('// INSERT TICK',tick)),/PASS OTA refresh and refusal/);
});
test('GATT diagnostics reads use the atomic OTA snapshot',()=>{
  const diagnostics=source.slice(source.indexOf('void updateDiagnosticsCharacteristic() {'),source.indexOf('void stopStreaming(ErrorCode reason) {'));
  assert.match(diagnostics,/otaBusySnapshot.load\(\)/);
  assert.doesNotMatch(diagnostics,/otaBusy\(\)|otaSession\./);
});
