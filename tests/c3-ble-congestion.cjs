'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.resolve(__dirname,'..');
const file=p=>fs.readFileSync(path.join(root,p),'utf8');
const session=file('firmware/shared/audio-session.cpp');
const sender=file('firmware/shared/audio-transport.cpp');
const source=file('synap_esp32s3/synap_esp32s3.ino');
const fn=session.slice(session.indexOf('bool configureTransportFromPeerMtu() {'),session.indexOf('void startStreaming(uint8_t version) {'));
const fixture=file('tests/c3-ble-congestion.cpp').replace('// INSERT TRANSPORT',fn);
for(const [c3,flags] of [[true,['-DCONFIG_IDF_TARGET_ESP32C3=1','-DSYNAP_CHAKSHU=0']],[false,['-DCONFIG_IDF_TARGET_ESP32C3=0','-DSYNAP_CHAKSHU=0']]]){
 test(`C3 constrained BLE mode and S3 lossless default (C3=${c3})`,()=>{
  assert.match(nativeTest(fixture,flags),/PASS C3 ADPCM congestion-safe/);
 });
}
test('C3 materialized firmware retains ADPCM protocol v3 and permanent congestion cutoff',()=>{
 const c3=materialize(source,'esp32c3-supermini-4m');
 assert.match(c3,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU\s+pcmTransport=false;/);
 assert.match(c3,/AUDIO_PROTOCOL_VERSION = 3/);
 assert.match(sender,/consecutiveRejectedFrames>=4u/);
 assert.match(sender,/requestStreamError\(ErrorCode::TRANSPORT_CHANGED, frame.generation\)/);
 assert.match(sender,/if \(congestionGeneration!=frame.generation\)/);
 assert.match(sender,/else if \(sent\) consecutiveRejectedFrames=0;/);
});
