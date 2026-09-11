const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

const base=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const source=base;

test('production source exposes dedicated asynchronous event characteristic',()=>{
  assert.match(source,/EVENT_CHAR_UUID "4fa1234e-0000-1000-8000-00805f9b34fb"/);
  assert.match(source,/BLECharacteristic\* eventCharacteristic = nullptr;/);
  assert.match(source,/eventCharacteristic=service->createCharacteristic\(EVENT_CHAR_UUID/);
  assert.match(source,/eventCharacteristic->addDescriptor\(new BLE2902\(\)\)/);
});

test('battery and power publish to the event channel; battery also reaches control subscribers',()=>{
  const eventNotifies=(source.match(/eventCharacteristic->notify\(\);/g)||[]).length;
  assert.equal(eventNotifies,2);
  assert.match(source,/controlCharacteristic->setValue\(value,sizeof\(value\)\);/);
  assert.match(source,/POWER_EVENT_MAGIC/);
  assert.match(source,/BATTERY_EVENT_MAGIC/);
});
