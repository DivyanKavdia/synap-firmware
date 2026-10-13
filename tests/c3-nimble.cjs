'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');

test('Odyssey C3 materializes onto NimBLE without changing its media protocol',()=>{
  const c3=materialize(source,'esp32c3-supermini-4m');
  assert.match(c3,/#include <NimBLEDevice\.h>/);
  assert.doesNotMatch(c3,/#include <BLEDevice\.h>/);
  assert.doesNotMatch(c3,/BLE2902|getConnId\(\)|getData\(\)|BLECharacteristic::PROPERTY_/);
  assert.match(c3,/std::atomic<uint16_t> odysseyConnectionHandle/);
  assert.match(c3,/std::atomic<bool> odysseyAudioSubscribed/);
  assert.match(c3,/NIMBLE_PROPERTY::READ/);
  assert.match(c3,/OdysseyTransfer::ble\(service\)/);
  assert.match(c3,/namespace OdysseyWifi/);
  assert.match(c3,/audioCharacteristic->notify\(packet,AUDIO_HEADER_BYTES\+length,odysseyConnectionHandle\.load\(\)\)/);
  assert.match(c3,/odysseyAudioSubscribed\.load\(\).*AUDIO_NOT_SUBSCRIBED/s);
});
