'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..'),read=p=>fs.readFileSync(path.join(root,p),'utf8');
test('C3 SD mutex is released before a recording task deletes itself',()=>{
  const source=read('firmware/shared/odyssey-sd-recording.cpp');
  const take=source.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
  const task=source.split('static void odysseyRecordTask(void*) {')[1].split('bool odysseyPrepareForConnectedStreaming')[0];
  assert.match(take,/OdysseySdGuard storage;/);
  assert.doesNotMatch(take,/vTaskDelete/);
  assert.match(task,/odysseyRecordTake\(\);[\s\S]*odysseyRecording=false;[\s\S]*vTaskDelete\(nullptr\)/);
});
test('C3 reconnect never stops SD capture; START performs explicit handoff',()=>{
  const ble=read('firmware/shared/ble-control.cpp');
  const connect=ble.split('void onConnect(BLEServer* server) override {')[1].split('void onDisconnect(BLEServer* server) override {')[0];
  assert.doesNotMatch(connect,/odysseyStopRequested\s*=\s*true/);
  const session=read('firmware/shared/audio-session.cpp');
  assert.match(session,/odysseyPrepareForConnectedStreaming\(1500u\)/);
});
test('C3 supports background mounting without remounting under active capture',()=>{
  const detect=read('firmware/shared/odyssey-sd-detect.cpp');
  const transfer=read('firmware/shared/odyssey-sd-transfer.cpp');
  const recorder=read('firmware/shared/odyssey-sd-recording.cpp');
  assert.match(detect,/odysseySdRequestRecovery\(\)/);
  assert.match(transfer,/automaticRetries<3/);
  assert.match(transfer,/!odysseyRecording\.load\(\)/);
  assert.match(transfer,/!streamingEnabled\.load\(\)/);
  assert.match(recorder,/odysseySdRequestRecovery\(\)/);
});
test('BLE STOP and reconnect cannot release SD-owned I2S or block the control task',()=>{
  const session=read('firmware/shared/audio-session.cpp');
  const stop=session.split('void stopStreaming(ErrorCode reason) {')[1].split('bool configureTransportFromPeerMtu() {')[0];
  assert.match(stop,/if \(!odysseyRecording\.load\(\)\) stopMicrophone\(\)/);
  assert.match(stop,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU/);
});
