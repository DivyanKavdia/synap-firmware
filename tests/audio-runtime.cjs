'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('audio bytes remain stable; all MTUs retain frame pacing without final-fragment spinning',()=>{
  const codec=source.slice(source.indexOf('static const uint16_t IMA_STEP_TABLE'),source.indexOf('void transmitterTask(void* parameter) {'));
  const transport=source.slice(source.indexOf('bool configureTransportFromPeerMtu() {'),source.indexOf('void startStreaming(uint8_t version) {'));
  const fixture=fs.readFileSync(path.join(__dirname,'audio-runtime.cpp'),'utf8');
  const result=nativeTest(fixture.replace('// INSERT CODEC AND TRANSPORT',transport+codec));
  assert.match(result,/PASS runtime; codec golden=3749bea1db6af550/);
});
