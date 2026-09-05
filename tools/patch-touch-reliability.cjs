'use strict';
const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  // TTP223 OUT is a push-pull digital signal. Do not add an ESP pulldown.
  out=replaceOnce(out,
`  pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN);`,
`  pinMode(TOUCH_INPUT_PIN, INPUT);`,
  'TTP223 input mode');

  out=replaceOnce(out,
`constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 3000;`,
`constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 5000;`,
  'five-second sleep hold');

  // Centralize the wake source so target materialization can translate the one S3
  // EXT1 call into the ESP32-C3 GPIO wake API without duplicate anchors.
  out=replaceOnce(out,
`  esp_sleep_enable_ext1_wakeup(1ULL<<TOUCH_INPUT_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);\n  esp_deep_sleep_start();`,
`  armTouchWakeAndSleep();`,
  'centralized touch sleep entry');

  out=replaceOnce(out,
`void enterDeepSleep(const char* reason) {`,
`void armTouchWakeAndSleep() {
  esp_sleep_enable_ext1_wakeup(1ULL<<TOUCH_INPUT_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

bool confirmTouchWakeHold() {
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bool touchWake=(cause==ESP_SLEEP_WAKEUP_EXT1);
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  touchWake=touchWake || (cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
  if (!touchWake) return true;

  Serial.println("[TOUCH] wake detected; hold for 5 seconds to stay awake");
  const uint32_t started=millis();
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
    if (uint32_t(millis()-started)>=TOUCH_SLEEP_HOLD_MS) {
      Serial.println("[TOUCH] 5 second wake hold confirmed");
      // Consume the wake gesture completely. Normal gesture timing starts only
      // after the user releases the sensor.
      while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) delay(10);
      touchRawState=false;touchStableState=false;touchPressedAt=0;touchFirstTapAt=0;
      touchChangedAt=millis();
      return true;
    }
    delay(10);
  }

  Serial.println("[TOUCH] wake hold too short; returning to deep sleep");
  delay(40);
  armTouchWakeAndSleep();
  return false;
}

void enterDeepSleep(const char* reason) {`,
  'touch wake confirmation helpers');

  out=replaceOnce(out,
`  statusLed.clear();
  statusLed.show();
  disconnectedAt=millis();`,
`  statusLed.clear();
  statusLed.show();
  if (!confirmTouchWakeHold()) return;
  disconnectedAt=millis();`,
  'five-second wake confirmation during setup');

  // Replace all overlapping touch gestures with one state machine:
  // idle: hold >=2 s and <5 s, then release -> START
  // streaming: double tap -> STOP; every other short/medium gesture is ignored
  // any non-OTA state: hold >=5 s, then release -> deep sleep
  const start=out.indexOf('void pollTouchControl() {');
  const end=out.indexOf('\n}\n',start);
  if(start<0||end<0)throw new Error('Missing firmware anchor: touch control function');
  const replacement=`void pollTouchControl() {
  constexpr uint16_t TOUCH_START_HOLD_MS = 2000;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 450;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 650;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool sleepAfterStop = false;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  if (sleepAfterStop && !streaming && !raw && !otaBusy()) {
    sleepAfterStop=false;
    enterDeepSleep("touch-hold-after-stop");
    return;
  }

  if (connected!=lastConnectedState || streaming!=lastStreamingState) {
    lastConnectedState=connected;
    lastStreamingState=streaming;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    touchFirstTapAt=0;
  }

  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;
        touchFirstTapAt=0;
        Serial.println("[TOUCH] ignored during state lockout");
        return;
      }
      touchPressedAt=now;
      Serial.printf("[TOUCH] press gpio=%u streaming=%u connected=%u\\n",
        unsigned(TOUCH_INPUT_PIN),streaming?1u:0u,connected?1u:0u);
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held) return;

    if (held>=TOUCH_SLEEP_HOLD_MS && !otaBusy()) {
      touchFirstTapAt=0;
      Serial.printf("[TOUCH] %ums hold -> SLEEP\\n",unsigned(held));
      if (streamingEnabled.load()) {
        sleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("touch-hold");
      }
      return;
    }

    if (streamingEnabled.load()) {
      if (held<=TOUCH_TAP_MAX_MS && !otaBusy()) {
        if (touchFirstTapAt && uint32_t(now-touchFirstTapAt)<=TOUCH_DOUBLE_TAP_MS) {
          touchFirstTapAt=0;
          touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
          Serial.println("[TOUCH] double tap while recording -> STOP");
          queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
        } else {
          touchFirstTapAt=now;
          Serial.println("[TOUCH] first recording tap; waiting for second tap");
        }
      } else {
        touchFirstTapAt=0;
        Serial.printf("[TOUCH] recording gesture %ums ignored\\n",unsigned(held));
      }
      return;
    }

    // Double tap while idle intentionally does nothing. Starting requires one
    // deliberate two-second hold and release; a five-second hold is reserved for sleep.
    touchFirstTapAt=0;
    if (held>=TOUCH_START_HOLD_MS && held<TOUCH_SLEEP_HOLD_MS && connected && !otaBusy()) {
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      Serial.printf("[TOUCH] %ums hold -> START\\n",unsigned(held));
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
    } else {
      Serial.printf("[TOUCH] idle gesture %ums ignored\\n",unsigned(held));
    }
  }
}`;
  out=out.slice(0,start)+replacement+out.slice(end+3);

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-touch-reliability.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap touch controls: 2s start, double-tap stop, 5s sleep/wake');
}
module.exports={patch};
