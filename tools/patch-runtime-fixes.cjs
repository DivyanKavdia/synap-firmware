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
`bool batteryCritical() {
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
}`,
`#ifndef SYNAP_BATTERY_MONITOR_ENABLE
#define SYNAP_BATTERY_MONITOR_ENABLE 0
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
`constexpr uint8_t RGB_LED_PIN = 48;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;`,
`constexpr uint8_t RGB_LED_PIN = 48;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr uint8_t TOUCH_PIN = 7;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 45;
#endif`,'TTP223 pin');

  out=replaceOnce(out,
`bool restartAdvertising = false;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;`,
`bool restartAdvertising = false;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
bool touchInitialized=false, touchRawState=false, touchStableState=false;
uint32_t touchChangedAt=0;
#endif
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;`,'TTP223 state');

  out=replaceOnce(out,
`void processCommand(uint8_t command, uint8_t version);
void controlTask(void* parameter);`,
`void processCommand(uint8_t command, uint8_t version);
void pollTouchSensor();
void controlTask(void* parameter);`,'TTP223 prototype');

  out=replaceOnce(out,
`void controlTask(void* parameter) {
  (void)parameter;`,
`void pollTouchSensor() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  const bool raw=digitalRead(TOUCH_PIN)==HIGH;
  const uint32_t now=millis();
  if(!touchInitialized) {
    touchInitialized=true;touchRawState=touchStableState=raw;touchChangedAt=now;
    return;
  }
  if(raw!=touchRawState) { touchRawState=raw;touchChangedAt=now; }
  if(raw==touchStableState || uint32_t(now-touchChangedAt)<TOUCH_DEBOUNCE_MS) return;
  touchStableState=raw;
  if(!touchStableState || !deviceConnected.load() || otaBusy()) return;
  if(streamingEnabled.load()) stopStreaming();
  else startStreaming(PROTOCOL_VERSION);
#endif
}

void controlTask(void* parameter) {
  (void)parameter;`,'TTP223 poller');

  out=replaceOnce(out,
`#endif
    otaTick();
  }
}`,
`#endif
    pollTouchSensor();
    otaTick();
  }
}`,'TTP223 control loop');

  out=replaceOnce(out,
`  statusLed.begin();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);`,
`  statusLed.begin();
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  pinMode(TOUCH_PIN, INPUT);
  touchRawState=touchStableState=digitalRead(TOUCH_PIN)==HIGH;
  touchChangedAt=millis();touchInitialized=true;
#endif
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);`,'TTP223 setup');

  out=replaceOnce(out,
`    case CMD_STOP:
      stopStreaming();
      break;`,
`    case CMD_STOP:
      // STOP can arrive while an audio notification is still inside the BLE stack.
      // First invalidate capture/transmit work without issuing a second notification,
      // then give the transmitter one short scheduling window to leave notify(), and
      // only then publish CONNECTED_IDLE. This avoids iOS/Bluefy dropping GATT when
      // audio + control notifications collide at the end of a take.
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
  console.log('Patched Synap runtime safeguards and ESP32-S3 TTP223 touch control');
}
module.exports={patch};