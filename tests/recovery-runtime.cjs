'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('bounded recovery uses the production ring and negotiated ownership; unsafe resumes never stream',()=>{
  const code=source.slice(source.indexOf('// Optional recovery protocol.'),source.indexOf('void stopStreaming(ErrorCode reason) {'));
  const stop=source.slice(source.indexOf('void stopStreaming(ErrorCode reason) {'),source.indexOf('bool configureTransportFromPeerMtu() {'));
  const start=source.slice(source.indexOf('void startStreaming(uint8_t version) {'),source.indexOf('void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream) {'));
  const commands=source.slice(source.indexOf('void processCommand(uint8_t command, uint8_t version) {'),source.indexOf('void reconcileConnection() {'));
  assert.match(nativeTest(fs.readFileSync(path.join(__dirname,'recovery-runtime.cpp'),'utf8').replace('// INSERT RECOVERY',code)
    .replace('// INSERT RECORDING COMMANDS',stop+start+commands)),/PASS recovery/);
});
