'use strict';
const { test } = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const { nativeTest } = require('./support/native.cjs');
const { patch } = require('../tools/patch-arduino-ble.cjs');
test('unsupported BLE library cannot be silently patched', () => {
  assert.throws(() => patch('BLECharacteristic.cpp', 'other release'), /Unsupported Arduino BLE source/);
});
test('pinned S3/C3 GATT handler reads live status and preserves every original command',
  { skip: !process.env.SYNAP_ARDUINO_BLE_SRC }, () => {
    const library = fs.readFileSync(path.join(process.env.SYNAP_ARDUINO_BLE_SRC, 'BLECharacteristic.cpp'), 'utf8');
    const fixed = patch('BLECharacteristic.cpp', library);
    assert.equal(patch('BLECharacteristic.cpp', fixed), fixed);
    assert.match(fixed, /onWriteValue\(pCharacteristic, &desc, buf, len\)/);
    const handler = fixed.slice(fixed.indexOf('int BLECharacteristic::handleGATTServerEvent(uint16_t'),
      fixed.indexOf('\n}\n', fixed.indexOf('int BLECharacteristic::handleGATTServerEvent(uint16_t')) + 2);
    assert(handler.length > 100);
    const source = fs.readFileSync('firmware/shared/ble-control.cpp', 'utf8');
    const callbacks = source.slice(source.indexOf('class ControlCallbacks'), source.indexOf('class AudioCallbacks'));
    const fixture = fs.readFileSync(path.join(__dirname, 'arduino-ble-gatt.cpp'), 'utf8');
    assert.match(nativeTest(fixture.replace('// INSERT CALLBACKS', callbacks).replace('// INSERT HANDLER', handler),
      ['-Wno-unused-parameter']), /PASS pinned S3\/C3/);
  });
