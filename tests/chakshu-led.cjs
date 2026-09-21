'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
test('Chakshu status and boot do not drive the shared SD chip-select as an LED',()=>{
 const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
 const led=source.slice(source.indexOf('void updateStatusLed(bool force) {'),source.indexOf('void setDeviceState('));
 assert.doesNotMatch(led,/digitalWrite|pinMode|statusLed\./);
 assert.doesNotMatch(source,/(?:digitalWrite|pinMode)\(\s*(?:21|LED_BUILTIN)\s*,/);
 assert.match(source,/constexpr uint8_t SD_SCK=7,SD_MISO=8,SD_MOSI=9,SD_CS=21/);
 assert.match(source,/pinMode\(SD_CS,OUTPUT\)/);
 assert.match(source,/digitalWrite\(SD_CS,HIGH\)/);
 assert.match(source,/SD\.begin\(SD_CS,SPI/);
});
