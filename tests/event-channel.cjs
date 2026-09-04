const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:events}=require('../tools/patch-event-channel.cjs');

const base=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const source=events(runtime(prepare(base)));

test('production source exposes dedicated asynchronous event characteristic',()=>{
  assert.match(source,/EVENT_CHAR_UUID "4fa1234e-0000-1000-8000-00805f9b34fb"/);
  assert.match(source,/BLECharacteristic\* eventCharacteristic = nullptr;/);
  assert.match(source,/eventCharacteristic=service->createCharacteristic\(EVENT_CHAR_UUID/);
  assert.match(source,/eventCharacteristic->addDescriptor\(new BLE2902\(\)\)/);
});

test('battery and memory publish to event channel with legacy control fallback',()=>{
  const eventNotifies=(source.match(/eventCharacteristic->notify\(\);/g)||[]).length;
  assert.equal(eventNotifies,2);
  assert.match(source,/Compatibility path for PWA builds that predate EVENT_CHAR_UUID/);
  assert.match(source,/MEMORY_EVENT_MAGIC/);
  assert.match(source,/BATTERY_EVENT_MAGIC/);
});
