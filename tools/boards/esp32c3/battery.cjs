'use strict';
const {replaceOnce,replaceFunctionBlock}=require('../../target-source.cjs');

function apply(source){
  let out=source;
  out=replaceOnce(out,`#if CONFIG_IDF_TARGET_ESP32S3
#define SYNAP_BATTERY_MONITOR_ENABLE 1
#else
#define SYNAP_BATTERY_MONITOR_ENABLE 0
#endif`, '#define SYNAP_BATTERY_MONITOR_ENABLE 1', 'C3 battery telemetry');
  out=replaceOnce(out,`  // Measured calibration for the 1M/470k divider: 1.32 V ADC = 4.13 V cell (raw 1544).
  constexpr uint32_t BATTERY_CAL_ADC_MV = 1320u;
  constexpr uint32_t BATTERY_CAL_CELL_MV = 4130u;
  const uint32_t cellMv=(adcMv*BATTERY_CAL_CELL_MV + BATTERY_CAL_ADC_MV/2u)/BATTERY_CAL_ADC_MV;`,
  `  // Provisional meter reference: 1.36 V at the junction = 3.99 V cell.
  // Assumes calibrated ADC millivolts match the junction measurement.
  const uint32_t cellMv=(adcMv*3990u + 680u)/1360u;`, 'C3 measured divider reference');
  out=replaceOnce(out,`  // GPIO1 is calibrated at 1.32 V ADC for a 4.13 V cell on the 1M/470k divider.
  // 6 dB attenuation comfortably covers the expected range while retaining resolution.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);`,
  `  // Keep the established ADC range while validating the measured divider ratio.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);`, 'C3 ADC input range');
  out=replaceOnce(out,'  // Production calibration: DMM 4.13 V, ADC 1.32 V, raw 1544 = full charge.',
    '  // LiPo estimate with the selected full-charge reference at 4.15 V.', 'C3 percentage estimate');
  out=replaceOnce(out,`  if (mv>=4130) return 100;
  if (mv>=4050) return 90 + uint32_t(mv-4050)*10/80;`,
    `  if (mv>=4150) return 100;
  if (mv>=4050) return 90 + uint32_t(mv-4050)*10/100;`, 'C3 full-charge percentage');
  out=replaceFunctionBlock(out,'bool batteryCritical() {','void publishBatteryEvent',`bool batteryCritical() {
  // Keep automatic sleep/OTA lockout off while validating the new C3 divider.
  return false;
}

`, 'C3 telemetry-only battery trial');
  out=replaceOnce(out,`    // A single averaged conversion is sufficient for UI availability. Critical
    // actions still require multiple corroborating samples via batteryCritical().`,
    '    // Expose valid readings immediately; this C3 trial does not enforce cutoff.', 'C3 battery availability');

  return out;
}

module.exports={apply};
