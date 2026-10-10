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
    batteryAdcMv: 1, batteryCellMv: 2, batteryFullMv: 4200,
  });
  assert.match(boot,
    /#if CONFIG_IDF_TARGET_ESP32C3\s+analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_11db\);\s+#else\s+analogSetPinAttenuation\(BATTERY_ADC_PIN, ADC_6db\);\s+#endif/);
  assert.match(battery, /markOdysseySdBatteryDividerPresent/);
});

test('Rev K SD and standard C3 are both x2; S3 remains independently calibrated', () => {
  const s3 = devices.find((d) => d.id === 'esp32s3-fh4r2-qspi-4m');
  // Standard C3: 1 MOhm high / 1 MOhm low => x2.
  assert.equal(target.hardware.batteryCellMv / target.hardware.batteryAdcMv, 2);
  // Rev K SD C3: R1=R2=470k => x2 (see final/BOM.csv and NETLIST.csv).
  assert.equal(target.hardware.sdBatteryCalibration.batteryCellMv /
    target.hardware.sdBatteryCalibration.batteryAdcMv, 2);
  // S3: separately calibrated around 1 MOhm / 470 kOhm, but NOT C3's GPIO.
  assert.equal(s3.hardware.battery, 8);
  assert.equal(s3.hardware.batteryAttenuation, 'ADC_6db');
  assert(Math.abs(s3.hardware.batteryCellMv / s3.hardware.batteryAdcMv - 1470 / 470) < 0.005);
});

test('Rev K 470k/470k divider fits ADC_11db across the 1S cell range', () => {
  const top = 470000, bottom = 470000;
  for (const cellMv of [2800, 3300, 3700, 4200, 4350]) {
    const pinMv = cellMv * bottom / (top + bottom);
    assert(pinMv < 2500, 'C3 ADC_11db must cover battery divider output');
    assert(Math.abs(Math.round(pinMv * 2) - cellMv) <= 2);
  }
});
