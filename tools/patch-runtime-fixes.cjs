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
`constexpr uint32_t BATTERY_DIVIDER_BOTTOM_OHMS = 330000u;`,
`constexpr uint32_t BATTERY_DIVIDER_BOTTOM_OHMS = 470000u;`,
  'S3 battery divider bottom resistor');

  out=replaceOnce(out,
`// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 330 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible/unstable readings are treated unavailable.`,
`// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 470 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible/unstable readings are treated unavailable.`,
  'battery divider wiring comment');

  out=replaceOnce(out,
`bool batteryCritical() {
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
}`,
`#ifndef SYNAP_BATTERY_MONITOR_ENABLE
#if CONFIG_IDF_TARGET_ESP32S3
#define SYNAP_BATTERY_MONITOR_ENABLE 1
#else
#define SYNAP_BATTERY_MONITOR_ENABLE 0
#endif
#endif
bool batteryCritical() {
#if SYNAP_BATTERY_MONITOR_ENABLE
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
#else
  return false;
#endif
}`,'battery critical guard');

  out=replaceOnce(out,
`void sampleBattery(bool force) {
  const uint32_t now=millis();`,
`void sampleBattery(bool force) {
#if !SYNAP_BATTERY_MONITOR_ENABLE
  (void)force;
  batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
  batteryMillivolts=0;batteryPercent=0;
  return;
#else
  const uint32_t now=millis();`,'battery sample guard start');

  out=replaceOnce(out,
`    batteryAvailable=batteryValidSamples>=2;`,
`    // A single averaged conversion is sufficient for UI availability. Critical
    // actions still require multiple corroborating samples via batteryCritical().
    batteryAvailable=batteryValidSamples>=1;`,
  'battery availability after first valid sample');

  out=replaceOnce(out,
`  publishBatteryEvent(true);
  updateStatusLed(true);
}

void enterDeepSleep`,
`  publishBatteryEvent(true);
  updateStatusLed(true);
#endif
}

void enterDeepSleep`,'battery sample guard end');

  out=replaceOnce(out,
`  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_2_5db);`,
`  // GPIO8 sees up to about 1.34 V from a 4.2 V cell through the 1M/470k divider.
  // 6 dB attenuation gives comfortable headroom and avoids top-end clipping.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);`,
  'battery ADC attenuation');

  out=replaceOnce(out,
`    case CMD_GET_STATUS:
      if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);`,
`    case CMD_GET_STATUS:
      if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      // The PWA requests status only after control notifications are subscribed,
      // so this guarantees fresh battery telemetry reaches the client on connect.
      sampleBattery(true);`,
  'battery sample on subscribed status request');

  out=replaceOnce(out,
`    case CMD_STOP:
      stopStreaming();
      break;`,
`    case CMD_STOP:
      streamingEnabled.store(false);
      ++streamGeneration;
      if (audioFrameQueue) xQueueReset(audioFrameQueue);
      setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
      vTaskDelay(pdMS_TO_TICKS(60));
      updateStatusCharacteristic(true);
      break;`,'safe stop acknowledgement');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-runtime-fixes.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  const out=patch(source);
  fs.writeFileSync(file,out);
  console.log('Patched Synap runtime safeguards');
}
module.exports={patch};