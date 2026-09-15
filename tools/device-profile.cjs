'use strict';
const { FLAGS, capabilityMask } = require('./targets.cjs');
const BEGIN = '// SYNAP_DEVICE_PROFILE_BEGIN';
const END = '// SYNAP_DEVICE_PROFILE_END';

function renderProfile(target) {
  const h = target.hardware;
  const define = (name, value) => `#define ${name} ${value}`;
  const optional = (name, value) => `#ifndef ${name}\n${define(name, value)}\n#endif`;
  return [
    BEGIN,
    '// Generated from devices/catalog.json; edit the catalog and reassemble.',
    define('SYNAP_CHAKSHU', Number(target.adapter === 'xiao-sense')),
    define('SYNAP_MODULE_ID', target.moduleId),
    define('DEVICE_NAME', JSON.stringify(target.advertisingName)),
    define('SYNAP_SUPPORTED_CAPABILITIES', capabilityMask(target)),
    ...Object.entries(FLAGS).map(([name, bit]) => define(`SYNAP_CAP_${name.toUpperCase()}`, bit)),
    optional('SYNAP_TOUCH_PIN', h.touch ?? -1),
    optional('SYNAP_BATTERY_ADC_PIN', h.battery ?? -1),
    optional('SYNAP_BATTERY_MONITOR_ENABLE', Number(h.battery !== null)),
    define('SYNAP_BATTERY_ENFORCE', Number(h.batteryCutoff)),
    define('SYNAP_BATTERY_FULL_MV', h.batteryFullMv ?? 4130),
    define('SYNAP_BATTERY_SCALE_NUMERATOR', h.batteryCellMv ?? 1),
    define('SYNAP_BATTERY_SCALE_DENOMINATOR', h.batteryAdcMv ?? 1),
    `constexpr uint8_t RGB_LED_PIN = ${h.led ?? 255};`,
    `constexpr int8_t I2S_BCLK_PIN = ${h.bclk ?? -1}, I2S_WS_PIN = ${h.ws ?? -1}, I2S_DATA_IN_PIN = ${h.data};`,
    `constexpr uint32_t IDLE_CPU_MHZ = ${h.idleMHz}, ACTIVE_CPU_MHZ = ${h.activeMHz};`,
    END,
  ].join('\n');
}
function profileBlock(source) {
  const first = source.indexOf(BEGIN), end = source.indexOf(END);
  if (first < 0 || end < first || source.indexOf(BEGIN, first + BEGIN.length) !== -1 ||
      source.indexOf(END, end + END.length) !== -1) throw Error('Missing or ambiguous device profile block');
  return source.slice(first, end + END.length);
}
function applyProfile(source, target) {
  return source.replace(profileBlock(source), renderProfile(target));
}
module.exports = { renderProfile, profileBlock, applyProfile };
