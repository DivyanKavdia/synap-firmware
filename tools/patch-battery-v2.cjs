const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  out=replaceOnce(out,
`constexpr uint8_t BATTERY_EVENT_VERSION = 1;`,
`constexpr uint8_t BATTERY_EVENT_VERSION = 2;`,
  'battery event version');

  out=replaceOnce(out,
`uint16_t batteryMillivolts = 0;\nuint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;`,
`uint16_t batteryMillivolts = 0, batteryAdcMillivolts = 0, batteryAdcRaw = 0;\nuint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;`,
  'battery telemetry globals');

  out=replaceOnce(out,
`  uint8_t value[8] = {BATTERY_EVENT_MAGIC, BATTERY_EVENT_VERSION, batteryPercent, 0, 0, 0, 0, 0};\n  if (batteryAvailable) value[3]|=0x01;\n  if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) value[3]|=0x02;\n  if (batteryCritical()) value[3]|=0x04;\n  value[4]=batteryMillivolts&255;value[5]=batteryMillivolts>>8;\n  value[6]=BATTERY_LOW_MV&255;value[7]=BATTERY_LOW_MV>>8;`,
`  // Battery event v2 extends the original 8-byte packet without changing its\n  // first 8 bytes. New fields expose the actual ADC measurement even when the\n  // reconstructed LiPo voltage is outside the trusted percentage window.\n  uint8_t value[12] = {BATTERY_EVENT_MAGIC, BATTERY_EVENT_VERSION, batteryPercent, 0, 0, 0, 0, 0, 0, 0, 0, 0};\n  if (batteryAvailable) value[3]|=0x01;\n  if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) value[3]|=0x02;\n  if (batteryCritical()) value[3]|=0x04;\n  value[4]=batteryMillivolts&255;value[5]=batteryMillivolts>>8;\n  value[6]=BATTERY_LOW_MV&255;value[7]=BATTERY_LOW_MV>>8;\n  value[8]=batteryAdcMillivolts&255;value[9]=batteryAdcMillivolts>>8;\n  value[10]=batteryAdcRaw&255;value[11]=batteryAdcRaw>>8;`,
  'battery event v2 payload');

  out=replaceOnce(out,
`  uint32_t total=0;\n  for (uint8_t i=0;i<8;++i) { total+=analogReadMilliVolts(BATTERY_ADC_PIN); delayMicroseconds(200); }\n  const uint32_t adcMv=total/8u;\n  const uint32_t cellMv=(adcMv*(BATTERY_DIVIDER_TOP_OHMS+BATTERY_DIVIDER_BOTTOM_OHMS)+`,
`  // High-value divider needs settling time. Throw away one conversion, then\n  // average both calibrated millivolts and raw ADC counts over 16 samples.\n  (void)analogRead(BATTERY_ADC_PIN);\n  delayMicroseconds(1200);\n  uint32_t mvTotal=0, rawTotal=0;\n  for (uint8_t i=0;i<16;++i) {\n    rawTotal+=analogRead(BATTERY_ADC_PIN);\n    mvTotal+=analogReadMilliVolts(BATTERY_ADC_PIN);\n    delayMicroseconds(250);\n  }\n  const uint32_t adcMv=mvTotal/16u;\n  const uint32_t adcRaw=rawTotal/16u;\n  batteryAdcMillivolts=uint16_t(adcMv>65535u?65535u:adcMv);\n  batteryAdcRaw=uint16_t(adcRaw>65535u?65535u:adcRaw);\n  const uint32_t cellMv=(adcMv*(BATTERY_DIVIDER_TOP_OHMS+BATTERY_DIVIDER_BOTTOM_OHMS)+`,
  'battery ADC acquisition');

  out=replaceOnce(out,
`  Serial.printf("[BATTERY] gpio=%u adc=%lumV cell=%umV available=%u percent=%u\\n",\n    static_cast<unsigned>(BATTERY_ADC_PIN),static_cast<unsigned long>(adcMv),static_cast<unsigned>(batteryMillivolts),\n    batteryAvailable?1u:0u,static_cast<unsigned>(batteryPercent));`,
`  Serial.printf("[BATTERY] gpio=%u raw=%u adc=%umV cell=%umV available=%u percent=%u\\n",\n    static_cast<unsigned>(BATTERY_ADC_PIN),static_cast<unsigned>(batteryAdcRaw),static_cast<unsigned>(batteryAdcMillivolts),\n    static_cast<unsigned>(batteryMillivolts),batteryAvailable?1u:0u,static_cast<unsigned>(batteryPercent));`,
  'battery serial diagnostics');

  out=replaceOnce(out,
`  pinMode(BATTERY_ADC_PIN, INPUT);\n  analogReadResolution(12);`,
`  pinMode(BATTERY_ADC_PIN, INPUT);\n  analogReadResolution(12);`,
  'battery input pin confirmation');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-battery-v2.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Added Synap battery telemetry v2');
}
module.exports={patch};
