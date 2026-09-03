const fs = require('node:fs');

function replaceOnce(source, before, after, label) {
  const first = source.indexOf(before);
  if (first < 0) throw new Error(`Missing firmware anchor: ${label}`);
  if (source.indexOf(before, first + before.length) >= 0) throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0, first) + after + source.slice(first + before.length);
}

function prepare(source) {
  let out = source;

  // Mobile browsers can suspend JavaScript while leaving the BLE link logically
  // connected. The old firmware destroyed the OTA handle after 45 seconds in
  // that state, making an otherwise resumable transfer restart from byte zero.
  // Retain both connected-idle and disconnected sessions for a bounded 15-minute
  // recovery window. A validated RESUME command still has to match session,
  // image digest and public device ID before a new connection can take ownership.
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
constexpr uint8_t TOUCH_INPUT_PIN = SYNAP_TOUCH_PIN;
constexpr uint8_t TOUCH_ACTIVE_LEVEL = SYNAP_TOUCH_ACTIVE_LEVEL;
constexpr uint16_t TOUCH_DEBOUNCE_MS = 35;
constexpr uint16_t TOUCH_DOUBLE_TAP_MS = 500;
// Status LED is intentionally off most of the time. Short, dim pulses make the
// state visible without turning the onboard WS2812 into a material battery load.
constexpr uint8_t LED_DIM = 4;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;`,
  'touch/LED constants');

  out = replaceOnce(out,
`esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;`,
`esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
uint32_t touchFirstTapAt = 0;
bool touchRawState = false, touchStableState = false;
uint32_t touchChangedAt = 0;
uint32_t lastLedPattern = UINT32_MAX;`,
  'interaction globals');

  out = replaceOnce(out,
`void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusCharacteristic(bool notify);`,
`void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusLed(bool force = false);
void pollTouchControl();
void updateStatusCharacteristic(bool notify);`,
  'interaction prototypes');

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
  if (otaBusy()) {
    // Firmware update: two short amber pulses every 1.4 s.
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
  } else if (deviceState == DeviceState::DISCONNECTED) {
    // Powered but not connected: one red pulse every 5 s.
    if (now%5000u<35u) r=LED_DIM;
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {
    // Connected/ready: one blue pulse every 6 s.
    if (now%6000u<30u) b=LED_DIM;
  } else if (deviceState == DeviceState::STREAMING) {
    // Recording: a slightly more frequent green pulse.
    if (now%1800u<45u) g=LED_DIM+1;
  } else {
    // Error: magenta pulse every 1.2 s.
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

void pollTouchControl() {
  const uint32_t now=millis();
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState && deviceConnected.load() && !otaBusy()) {
      if (!streamingEnabled.load()) {
        // A single tap while connected and idle starts recording immediately.
        touchFirstTapAt=0;
        queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
      } else if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)<=TOUCH_DOUBLE_TAP_MS) {
        // While recording, require a deliberate second tap to stop.
        touchFirstTapAt=0;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        touchFirstTapAt=now;
      }
    }
  }
  if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)>TOUCH_DOUBLE_TAP_MS) touchFirstTapAt=0;
}
`,
  'state LED implementation');

  out = replaceOnce(out,
`    otaTick();
  }
}`,
`    pollTouchControl();
    otaTick();
    updateStatusLed();
  }
}`,
  'control loop interaction tick');

  out = replaceOnce(out,
`  bootResetReason=esp_reset_reason();
  statusLed.begin();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);`,
`  bootResetReason=esp_reset_reason();
  pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN);
  touchRawState=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  touchStableState=touchRawState;
  touchChangedAt=millis();
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);`,
  'touch setup');

  return out;
}

if (require.main === module) {
  const args=process.argv.slice(2);
  const check=args[0]==='--check';
  const file=check?args[1]:args[0];
  if (!file) throw new Error('Usage: node tools/prepare-interactions.cjs [--check] <sketch>');
  const source=fs.readFileSync(file,'utf8');
  const prepared=prepare(source);
  for (const marker of ['pollTouchControl','TOUCH_DOUBLE_TAP_MS','Firmware update: two short amber pulses','Connected/ready: one blue pulse','900000u','Screen lock/background suspension is expected on mobile']) {
    if (!prepared.includes(marker)) throw new Error('Prepared firmware missing '+marker);
  }
  if (!check) fs.writeFileSync(file,prepared);
  console.log(check?'PASS: touch, low-power LED and OTA suspension preparation':'Prepared touch controls, low-power LED states and OTA suspension recovery');
}

module.exports={prepare};
