'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('capture blocks until notified and rejects a stopped frame before queueing',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const task=source.slice(source.indexOf('void acquisitionTask(void* parameter) {'),source.indexOf('static const uint16_t IMA_STEP_TABLE'));
  const fixture=fs.readFileSync(path.join(__dirname,'capture-wakeup.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT ACQUISITION TASK',task)),/PASS capture wake, stop cancellation and restart/);
});
