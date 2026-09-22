'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
test('Chakshu uses the external GPIO4 NeoPixel and never repurposes SD chip-select as status LED',()=>{
 const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
 const led=source.slice(source.indexOf('void updateStatusLed(bool force) {'),source.indexOf('void setDeviceState('));
 assert.match(source,/constexpr uint8_t RGB_LED_PIN = 4;/);
 assert.match(source,/Adafruit_NeoPixel statusLed\(1, RGB_LED_PIN, NEO_GRB \+ NEO_KHZ800\)/);
 assert.match(led,/statusLed\.setPixelColor/);
 assert.match(led,/statusLed\.show\(\)/);
 assert.doesNotMatch(source,/Adafruit_NeoPixel statusLed\(1,\s*(?:21|LED_BUILTIN)/);
 assert.match(source,/constexpr uint8_t SD_SCK=7,SD_MISO=8,SD_MOSI=9,SD_CS=21/);
 assert.match(source,/pinMode\(SD_CS,OUTPUT\)/);
 assert.match(source,/digitalWrite\(SD_CS,HIGH\)/);
 assert.match(source,/SD\.begin\(SD_CS,SPI/);
});
