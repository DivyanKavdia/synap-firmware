const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  // The production pendant hardware uses the original 1 MOhm / 330 kOhm divider.
  // Do not override BATTERY_DIVIDER_BOTTOM_OHMS from prepare-interactions.cjs.

  out=replaceOnce(out,
`constexpr uint32_t BATTERY_SAMPLE_MS = 30000u;`,
`constexpr uint32_t BATTERY_SAMPLE_MS = 15000u;`,
  'battery telemetry cadence');

  out=replaceOnce(out,
`// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 330 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible/unstable readings are treated unavailable.`,
`// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 330 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible readings are reported but marked unavailable.`,
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
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  updateStatusCharacteristic(false);
  lastBatteryPublishAt=now;`,
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Bluedroid may defer notification transmission. Give the 8-byte battery payload
  // a short window before restoring the 16-byte control/status value so mobile
  // browsers never observe the restored status in place of the queued battery event.
  vTaskDelay(pdMS_TO_TICKS(20));
  updateStatusCharacteristic(false);
  lastBatteryPublishAt=now;`,
  'battery notification handoff');

  out=replaceOnce(out,
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  updateStatusCharacteristic(false);
  memoryAckUntil=millis()+480u;`,
`  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Memory markers share the control characteristic with status packets. Mirror the
  // battery handoff so the 12-byte event is transmitted before status is restored.
  vTaskDelay(pdMS_TO_TICKS(20));
  updateStatusCharacteristic(false);
  memoryAckUntil=millis()+480u;`,
  'memory notification handoff');

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
`  } else {
    batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
    batteryMillivolts=0;batteryPercent=0;
  }
  publishBatteryEvent(true);`,
`  } else {
    // Preserve the reconstructed voltage even when it is outside the expected
    // LiPo range. The PWA can then distinguish bad wiring/ADC from missing BLE.
    batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
    batteryMillivolts=uint16_t(cellMv>65535u?65535u:cellMv);batteryPercent=0;
  }
  Serial.printf("[BATTERY] gpio=%u adc=%lumV cell=%umV available=%u percent=%u\\n",
    static_cast<unsigned>(BATTERY_ADC_PIN),static_cast<unsigned long>(adcMv),static_cast<unsigned>(batteryMillivolts),
    batteryAvailable?1u:0u,static_cast<unsigned>(batteryPercent));
  publishBatteryEvent(true);`,
  'battery invalid-reading diagnostics');

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
`  // GPIO8 sees up to about 1.04 V from a 4.2 V cell through the 1M/330k divider.
  // 6 dB attenuation comfortably covers the expected range while retaining resolution.
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