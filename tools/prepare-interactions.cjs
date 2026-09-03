const fs = require('node:fs');

function replaceOnce(source, before, after, label) {
  const first = source.indexOf(before);
  if (first < 0) throw new Error(`Missing firmware anchor: ${label}`);
  if (source.indexOf(before, first + before.length) >= 0) throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0, first) + after + source.slice(first + before.length);
}

function prepare(source) {
  let out = source;

  out = replaceOnce(out,
`#include <esp_system.h>`,
`#include <esp_system.h>
#include <esp_sleep.h>`,
  'sleep include');

  out = replaceOnce(out,
`// Synap 1.0.0 | ESP32-S3FH4R2 | Arduino-ESP32 3.3.5 + Adafruit NeoPixel.`,
`// Synap 0.0.1 | ESP32-S3FH4R2 | Arduino-ESP32 3.3.5 + Adafruit NeoPixel.`,
  'public firmware version comment');

  out = replaceOnce(out,
`  "SYNAP-FW:esp32s3-fh4r2-qspi-4m:1.0.0:" SYNAP_STRING(SYNAP_BUILD);`,
`  "SYNAP-FW:esp32s3-fh4r2-qspi-4m:0.0.1:" SYNAP_STRING(SYNAP_BUILD);`,
  'public firmware version identity');

  out = replaceOnce(out,
`      if (!connected || connection!=owner) {
        if (!orphanedAt) orphanedAt=now ? now : 1;
        if (uint32_t(now-orphanedAt)>120000u) fail(LINK_LOST);
        return;
      }
      orphanedAt=0;
      if (uint32_t(now-last)>45000u) fail(TIMED_OUT);`,
`      if (!connected || connection!=owner) {
        if (!orphanedAt) orphanedAt=now ? now : 1;
        if (uint32_t(now-orphanedAt)>900000u) fail(LINK_LOST);
        return;
      }
      orphanedAt=0;
      // Screen lock/background suspension is expected on mobile. Keep the flash
      // handle and exact persisted offset long enough for the PWA to resume.
      if (uint32_t(now-last)>900000u) fail(TIMED_OUT);`,
  'OTA phone-suspension retention');

  out = replaceOnce(out,
`constexpr uint8_t RGB_LED_PIN = 48;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;`,
`constexpr uint8_t RGB_LED_PIN = 48;
#ifndef SYNAP_TOUCH_PIN
#define SYNAP_TOUCH_PIN 7
#endif
#ifndef SYNAP_TOUCH_ACTIVE_LEVEL
#define SYNAP_TOUCH_ACTIVE_LEVEL HIGH
#endif
#ifndef SYNAP_BATTERY_ADC_PIN
#define SYNAP_BATTERY_ADC_PIN 8
#endif
constexpr uint8_t TOUCH_INPUT_PIN = SYNAP_TOUCH_PIN;
constexpr uint8_t TOUCH_ACTIVE_LEVEL = SYNAP_TOUCH_ACTIVE_LEVEL;
constexpr uint8_t BATTERY_ADC_PIN = SYNAP_BATTERY_ADC_PIN;
constexpr uint16_t TOUCH_DEBOUNCE_MS = 35;
constexpr uint16_t TOUCH_DOUBLE_TAP_MS = 500;
constexpr uint16_t TOUCH_LONG_PRESS_MS = 1200;
constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 3000;
constexpr uint32_t AUTO_SLEEP_DISCONNECTED_MS = 300000u;
constexpr uint32_t BATTERY_SAMPLE_MS = 30000u;
constexpr uint32_t BATTERY_DIVIDER_TOP_OHMS = 1000000u;
constexpr uint32_t BATTERY_DIVIDER_BOTTOM_OHMS = 330000u;
constexpr uint16_t BATTERY_LOW_MV = 3600;
constexpr uint16_t BATTERY_CRITICAL_MV = 3400;
constexpr uint8_t MEMORY_EVENT_MAGIC = 0xB6;
constexpr uint8_t MEMORY_EVENT_VERSION = 1;
constexpr uint8_t MEMORY_EVENT_REMEMBER = 1;
constexpr uint8_t BATTERY_EVENT_MAGIC = 0xB7;
constexpr uint8_t BATTERY_EVENT_VERSION = 1;
// TTP223 OUT is a digital, active-high push-pull signal by default.
// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 330 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible/unstable readings are treated unavailable.
// Status LED is intentionally off most of the time. Short, dim pulses make the
// state visible without turning the onboard WS2812 into a material battery load.
constexpr uint8_t LED_DIM = 4;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;`,
  'touch/power/LED constants');

  out = replaceOnce(out,
`esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;`,
`esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
uint32_t touchFirstTapAt = 0, touchPressedAt = 0, memoryAckUntil = 0, memoryEventCounter = 0;
bool touchRawState = false, touchStableState = false, touchLongSent = false, touchLongEligible = false, touchIdlePress = false;
uint32_t touchChangedAt = 0;
uint32_t lastLedPattern = UINT32_MAX;
uint32_t lastBatterySampleAt = 0, lastBatteryPublishAt = 0;
uint16_t batteryMillivolts = 0;
uint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;
bool batteryAvailable = false;`,
  'interaction/power globals');

  out = replaceOnce(out,
`void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusCharacteristic(bool notify);`,
`void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusLed(bool force = false);
void publishRememberEvent();
void publishBatteryEvent(bool force = false);
void sampleBattery(bool force = false);
bool batteryCritical();
void enterDeepSleep(const char* reason);
void powerTick();
void pollTouchControl();
void updateStatusCharacteristic(bool notify);`,
  'interaction/power prototypes');

  out = replaceOnce(out,
`    otaSession.packet(message.data,message.length,millis(),generation,streamingEnabled.load());`,
`    // Treat a confirmed critically-low battery like another busy condition: never
    // start or continue a new flash transaction when brownout margin is inadequate.
    otaSession.packet(message.data,message.length,millis(),generation,
      streamingEnabled.load() || batteryCritical());`,
  'battery-aware OTA guard');

  out = replaceOnce(out,
`  if (otaSession.state!=previous) {
    otaPublish(true);
    if (otaBusy()) {
      statusLed.setPixelColor(0,statusLed.Color(24,12,0));statusLed.show();
    } else setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }`,
`  if (otaSession.state!=previous) {
    otaPublish(true);
    if (otaBusy()) updateStatusLed(true);
    else setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }`,
  'OTA LED transition');

  out = replaceOnce(out,
`void setDeviceState(DeviceState state, ErrorCode error) {
  deviceState = state;
  errorCode = error;
  uint8_t r=0,g=0,b=0;
  if (state == DeviceState::DISCONNECTED) r=24;
  if (state == DeviceState::CONNECTED_IDLE) b=24;
  if (state == DeviceState::STREAMING) g=24;
  if (state == DeviceState::ERROR) { r=24; b=24; }
  statusLed.setPixelColor(0, statusLed.Color(r,g,b));
  statusLed.show();
}`,
`void updateStatusLed(bool force) {
  const uint32_t now = millis();
  uint8_t r=0,g=0,b=0;
  if (memoryAckUntil && static_cast<int32_t>(memoryAckUntil-now)>0) {
    const uint32_t phase=(memoryAckUntil-now)%240u;
    if (phase>120u) { g=LED_DIM+2; b=LED_DIM+2; }
  } else if (otaBusy()) {
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {
    // Low battery: subtle red double-pulse, without changing the functional state.
    const uint32_t phase=now%5000u;
    if (phase<40u || (phase>=180u && phase<220u)) r=LED_DIM;
  } else if (deviceState == DeviceState::DISCONNECTED) {
    if (now%5000u<35u) r=LED_DIM;
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {
    if (now%6000u<30u) b=LED_DIM;
  } else if (deviceState == DeviceState::STREAMING) {
    if (now%1800u<45u) g=LED_DIM+1;
  } else {
    if (now%1200u<70u) { r=LED_DIM; b=LED_DIM; }
  }
  const uint32_t pattern=(uint32_t(r)<<16)|(uint32_t(g)<<8)|b;
  if (!force && pattern==lastLedPattern) return;
  lastLedPattern=pattern;
  statusLed.setPixelColor(0,statusLed.Color(r,g,b));
  statusLed.show();
}

void setDeviceState(DeviceState state, ErrorCode error) {
  deviceState = state;
  errorCode = error;
  updateStatusLed(true);
}

uint8_t batteryPercentFromMillivolts(uint16_t mv) {
  if (mv>=4200) return 100;
  if (mv>=4000) return 80 + uint32_t(mv-4000)*20/200;
  if (mv>=3800) return 40 + uint32_t(mv-3800)*40/200;
  if (mv>=3600) return 15 + uint32_t(mv-3600)*25/200;
  if (mv>=3400) return 5 + uint32_t(mv-3400)*10/200;
  if (mv>=3200) return uint32_t(mv-3200)*5/200;
  return 0;
}

bool batteryCritical() {
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
}

void publishBatteryEvent(bool force) {
  if (!controlCharacteristic || !deviceConnected.load()) return;
  const uint32_t now=millis();
  if (!force && uint32_t(now-lastBatteryPublishAt)<BATTERY_SAMPLE_MS) return;
  uint8_t value[8] = {BATTERY_EVENT_MAGIC, BATTERY_EVENT_VERSION, batteryPercent, 0, 0, 0, 0, 0};
  if (batteryAvailable) value[3]|=0x01;
  if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) value[3]|=0x02;
  if (batteryCritical()) value[3]|=0x04;
  value[4]=batteryMillivolts&255;value[5]=batteryMillivolts>>8;
  value[6]=BATTERY_LOW_MV&255;value[7]=BATTERY_LOW_MV>>8;
  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  updateStatusCharacteristic(false);
  lastBatteryPublishAt=now;
}

void sampleBattery(bool force) {
  const uint32_t now=millis();
  if (!force && uint32_t(now-lastBatterySampleAt)<BATTERY_SAMPLE_MS) return;
  lastBatterySampleAt=now;
  uint32_t total=0;
  for (uint8_t i=0;i<8;++i) { total+=analogReadMilliVolts(BATTERY_ADC_PIN); delayMicroseconds(200); }
  const uint32_t adcMv=total/8u;
  const uint32_t cellMv=(adcMv*(BATTERY_DIVIDER_TOP_OHMS+BATTERY_DIVIDER_BOTTOM_OHMS)+
    BATTERY_DIVIDER_BOTTOM_OHMS/2u)/BATTERY_DIVIDER_BOTTOM_OHMS;
  if (cellMv>=2800u && cellMv<=4350u) {
    batteryMillivolts=uint16_t(cellMv);
    batteryPercent=batteryPercentFromMillivolts(batteryMillivolts);
    if (batteryValidSamples<255) ++batteryValidSamples;
    batteryAvailable=batteryValidSamples>=2;
    if (batteryMillivolts<=BATTERY_CRITICAL_MV) {
      if (batteryCriticalSamples<255) ++batteryCriticalSamples;
    } else batteryCriticalSamples=0;
  } else {
    batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
    batteryMillivolts=0;batteryPercent=0;
  }
  publishBatteryEvent(true);
  updateStatusLed(true);
}

void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load()) return;
  Serial.printf("[POWER] deep sleep: %s battery=%umV\n", reason?reason:"idle", unsigned(batteryMillivolts));
  statusLed.clear();statusLed.show();
  delay(25);
  BLEDevice::deinit(true);
  // Enter only after TTP223 has been released. Next active-high touch wakes and
  // restarts the firmware from setup(), restoring advertising automatically.
  esp_sleep_enable_ext1_wakeup(1ULL<<TOUCH_INPUT_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

void powerTick() {
  sampleBattery(false);
  if (batteryCritical() && !streamingEnabled.load() && !otaBusy()) {
    enterDeepSleep("critical-battery");
    return;
  }
  if (!deviceConnected.load() && !streamingEnabled.load() && !otaBusy() &&
      disconnectedAt && uint32_t(millis()-disconnectedAt)>=AUTO_SLEEP_DISCONNECTED_MS) {
    enterDeepSleep("disconnected-timeout");
  }
}

void publishRememberEvent() {
  if (!controlCharacteristic || !deviceConnected.load() || !streamingEnabled.load() || otaBusy()) return;
  uint8_t value[12] = {MEMORY_EVENT_MAGIC, MEMORY_EVENT_VERSION, MEMORY_EVENT_REMEMBER, 0};
  value[3] = (streamingEnabled.load()?0x01:0) | (deviceConnected.load()?0x02:0);
  const uint32_t counter=++memoryEventCounter, uptime=millis();
  put32le(value+4,counter);put32le(value+8,uptime);
  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  updateStatusCharacteristic(false);
  memoryAckUntil=millis()+480u;
  updateStatusLed(true);
}

void pollTouchControl() {
  const uint32_t now=millis();
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      touchPressedAt=now;touchLongSent=false;
      touchIdlePress=!streamingEnabled.load();
      touchLongEligible=deviceConnected.load() && streamingEnabled.load() && !otaBusy();
      if (!touchIdlePress && deviceConnected.load() && !otaBusy()) {
        if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)<=TOUCH_DOUBLE_TAP_MS) {
          touchFirstTapAt=0;
          queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
        } else touchFirstTapAt=now;
      }
    } else {
      const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
      const bool wasIdle=touchIdlePress;
      touchPressedAt=0;touchLongSent=false;touchLongEligible=false;touchIdlePress=false;
      if (wasIdle && !otaBusy()) {
        if (held>=TOUCH_SLEEP_HOLD_MS && !streamingEnabled.load()) {
          touchFirstTapAt=0;
          enterDeepSleep("touch-hold");
        } else if (held>=TOUCH_DEBOUNCE_MS && held<TOUCH_SLEEP_HOLD_MS &&
            deviceConnected.load() && !streamingEnabled.load()) {
          // Start on release so the same TTP223 can distinguish tap from 3 s sleep hold.
          touchFirstTapAt=0;
          queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
        }
      }
    }
  }
  if (touchStableState && touchLongEligible && !touchLongSent && touchPressedAt &&
      uint32_t(now-touchPressedAt)>=TOUCH_LONG_PRESS_MS && deviceConnected.load() &&
      streamingEnabled.load() && !otaBusy()) {
    touchLongSent=true;touchFirstTapAt=0;publishRememberEvent();
  }
  if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)>TOUCH_DOUBLE_TAP_MS) touchFirstTapAt=0;
}
`,
  'state LED and power implementation');

  out = replaceOnce(out,
`        case EventType::CONNECTED:
          restartAdvertising=false;
          peerMtu=23; attValueCapacity=20; chunksPerFrame=0; audioPayloadBytes=0;
          stopStreaming();
          break;`,
`        case EventType::CONNECTED:
          restartAdvertising=false;
          disconnectedAt=0;
          peerMtu=23; attValueCapacity=20; chunksPerFrame=0; audioPayloadBytes=0;
          stopStreaming();
          sampleBattery(true);
          break;`,
  'connected power state');

  out = replaceOnce(out,
`    otaTick();
  }
}`,
`    pollTouchControl();
    otaTick();
    powerTick();
    updateStatusLed();
  }
}`,
  'control loop interaction/power tick');

  out = replaceOnce(out,
`  bootResetReason=esp_reset_reason();
  statusLed.begin();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);`,
`  bootResetReason=esp_reset_reason();
  pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN);
  touchRawState=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  touchStableState=touchRawState;
  touchChangedAt=millis();
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_2_5db);
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  disconnectedAt=millis();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  sampleBattery(true);`,
  'touch/battery setup');

  return out;
}

if (require.main === module) {
  const args=process.argv.slice(2);
  const check=args[0]==='--check';
  const file=check?args[1]:args[0];
  if (!file) throw new Error('Usage: node tools/prepare-interactions.cjs [--check] <sketch>');
  const source=fs.readFileSync(file,'utf8');
  const prepared=prepare(source);
  for (const marker of ['Synap 0.0.1','SYNAP-FW:esp32s3-fh4r2-qspi-4m:0.0.1:','pollTouchControl','TOUCH_LONG_PRESS_MS','TOUCH_SLEEP_HOLD_MS','AUTO_SLEEP_DISCONNECTED_MS','BATTERY_EVENT_MAGIC','BATTERY_ADC_PIN','batteryCritical','enterDeepSleep','publishRememberEvent','900000u','Screen lock/background suspension is expected on mobile']) {
    if (!prepared.includes(marker)) throw new Error('Prepared firmware missing '+marker);
  }
  if (!check) fs.writeFileSync(file,prepared);
  console.log(check?'PASS: Synap touch, Remember This, deep sleep, battery and OTA power safeguards':'Prepared Synap power-managed firmware');
}

module.exports={prepare};
