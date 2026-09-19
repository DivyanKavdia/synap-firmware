'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {materialize}=require('../tools/materialize-target.cjs');

test('C3 GPIO8 NeoPixel uses the exact shared S3 RGB status engine',()=>{
  const source=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
  const s3=source,c3=materialize(source,'esp32c3-supermini-4m');
  assert.match(c3,/constexpr uint8_t RGB_LED_PIN = 8;/);
  assert.match(c3,/Adafruit_NeoPixel statusLed\(1, RGB_LED_PIN, NEO_GRB \+ NEO_KHZ800\)/);
  assert.doesNotMatch(c3,/C3 has a discrete LED|digitalWrite\(RGB_LED_PIN,on\?LOW:HIGH\)/);
  const block=(code)=>{
    const start=code.indexOf('void updateStatusLed(bool force) {');
    const end=code.indexOf('void setDeviceState(',start);
    return code.slice(start,end).replace(/\r/g,'');
  };
  assert.equal(block(c3),block(s3));
  assert.match(c3,/statusLed\.begin\(\);\s*statusLed\.clear\(\);\s*statusLed\.show\(\);/);
  assert.match(c3,/if \(otaBusy\(\)\)/);
  assert.match(c3,/deviceState == DeviceState::CONNECTED_IDLE/);
  assert.match(c3,/deviceState == DeviceState::STREAMING/);
});
