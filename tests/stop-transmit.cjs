'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('STOP acknowledgement follows actual transmitter completion',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const stop=source.slice(source.indexOf('void stopStreaming(ErrorCode reason) {'),source.indexOf('bool configureTransportFromPeerMtu() {'));
  const transmitter=source.slice(source.indexOf('void transmitterTask(void* parameter) {'),source.indexOf('void initializeBLE() {'));
  const fixture=fs.readFileSync(path.join(__dirname,'stop-transmit.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT STOP',stop).replace('// INSERT TRANSMITTER',transmitter),['-pthread']),/PASS STOP waits for transmit completion/);
});
