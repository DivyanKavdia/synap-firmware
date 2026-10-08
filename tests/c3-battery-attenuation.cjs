'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const boot = fs.readFileSync(path.join(root, 'firmware/shared/boot.cpp'), 'utf8');
const battery = fs.readFileSync(path.join(root, 'firmware/shared/battery.cpp'), 'utf8');
const devices = require('../devices/catalog.json').devices;
const target = devices.find((d) => d.id === 'esp32c3-supermini-4m');

test('Odyssey C3 uses its documented 11 dB ADC range without altering S3', () => {
  assert.equal(target.hardware.battery, 1);
  assert.equal(target.hardware.batteryAttenuation, 'ADC_11db');
  assert.deepEqual(target.hardware.sdBatteryCalibration, {
    batteryAdcMv: 470, batteryCellMv: 1470, batteryFullMv: 4200,
  });
  assert.match(boot,
    /#if CONFIG_IDF_TARGET_ESP32C3\s+analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_11db\);\s+#else\s+analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_6db\);\s+#endif/);
  assert.match(battery, /markOdysseySdBatteryDividerPresent/);
});

test('separate C3+SD, standard C3 and S3 resistor profiles stay distinct', () => {
  const s3 = devices.find((d) => d.id === 'esp32s3-fh4r2-qspi-4m');
  // Standard C3: 1 MOhm high / 1 MOhm low => x2.
  assert.equal(target.hardware.batteryCellMv / target.hardware.batteryAdcMv, 2);
  // SD C3: 1 MOhm high / 470 kOhm low => x1470/470.
  assert.equal(target.hardware.sdBatteryCalibration.batteryCellMv,
    1000 + target.hardware.sdBatteryCalibration.batteryAdcMv);
  assert.equal(target.hardware.sdBatteryCalibration.batteryAdcMv, 470);
  // S3: separately calibrated around 1 MOhm / 470 kOhm, but NOT C3's GPIO.
  assert.equal(s3.hardware.battery, 8);
  assert.equal(s3.hardware.batteryAttenuation, 'ADC_6db');
  assert(Math.abs(s3.hardware.batteryCellMv / s3.hardware.batteryAdcMv - 1470 / 470) < 0.005);
});

test('1 MOhm / 470 kOhm divider never exceeds the C3 11 dB input range for a 1S cell', () => {
  const top = 1000000, bottom = 470000;
  for (const cellMv of [2800, 3300, 3700, 4200, 4350]) {
    const pinMv = cellMv * bottom / (top + bottom);
    assert(pinMv < 2500, 'C3 ADC_11db must cover battery divider output');
    assert(Math.abs(Math.round(pinMv * 1470 / 470) - cellMv) <= 2);
  }
});
