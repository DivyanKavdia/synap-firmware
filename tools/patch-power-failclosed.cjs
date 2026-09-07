'use strict';
const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing fail-closed power anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous fail-closed power anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}
function replaceFunction(source,signature,replacement,label){
  const definition=signature.endsWith('{')?signature:signature+' {';
  const start=source.indexOf(definition);
  if(start<0)throw new Error(`Missing fail-closed power function: ${label}`);
  const brace=source.indexOf('{',start);
  if(brace<0)throw new Error(`Missing opening brace: ${label}`);
  let depth=0,end=-1;
  for(let i=brace;i<source.length;i++){
    if(source[i]==='{')depth++;
    else if(source[i]==='}'&&--depth===0){end=i+1;break;}
  }
  if(end<0)throw new Error(`Missing closing brace: ${label}`);
  return source.slice(0,start)+replacement+source.slice(end);
}

function patch(source){
  let out=source;

  out=replaceOnce(out,
`#include <esp_sleep.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/rtc_io.h>
#endif`,
`#include <esp_sleep.h>
#include <Preferences.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/rtc_io.h>
#endif`,
  'durable sleep storage include');

  out=replaceOnce(out,
`RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0;`,
`RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0;
RTC_DATA_ATTR uint32_t synapSleepRequestCounter = 0;
RTC_DATA_ATTR uint8_t synapLastSleepStage = 0;
RTC_DATA_ATTR uint8_t synapLastWakeCause = 0;
RTC_DATA_ATTR uint8_t synapLastGpioBeforeSleep = 0;
bool bootSleepWasLocked = false;
esp_sleep_wakeup_cause_t bootWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
constexpr char SYNAP_POWER_NAMESPACE[] = "synap-power";
constexpr char SYNAP_SLEEP_LOCK_KEY[] = "sleep-lock";
constexpr uint8_t SLEEP_STAGE_NONE = 0;
constexpr uint8_t SLEEP_STAGE_REQUESTED = 1;
constexpr uint8_t SLEEP_STAGE_LOCKED = 2;
constexpr uint8_t SLEEP_STAGE_GPIO_RELEASED = 3;
constexpr uint8_t SLEEP_STAGE_WAKE_ARMED = 4;
constexpr uint8_t SLEEP_STAGE_ENTERING = 5;
constexpr uint8_t SLEEP_STAGE_RESET_RECOVERY = 6;
constexpr uint8_t SLEEP_STAGE_WAKE_VALIDATING = 7;
constexpr uint8_t SLEEP_STAGE_WAKE_CONFIRMED = 8;
constexpr uint8_t SLEEP_STAGE_ABORTED = 9;

bool readDurableSleepLock() {
  Preferences prefs;
  if (!prefs.begin(SYNAP_POWER_NAMESPACE,true)) return false;
  const bool locked=prefs.getBool(SYNAP_SLEEP_LOCK_KEY,false);
  prefs.end();
  return locked;
}

bool writeDurableSleepLock(bool locked) {
  Preferences prefs;
  if (!prefs.begin(SYNAP_POWER_NAMESPACE,false)) return false;
  const bool ok=prefs.putBool(SYNAP_SLEEP_LOCK_KEY,locked)==1u;
  prefs.end();
  return ok;
}`,
  'durable sleep state');

  out=replaceFunction(out,'void armTouchWakeAndSleep()',`bool armTouchWakeSource() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t wakeError=ESP_FAIL;
#if CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_init(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  rtc_gpio_set_direction(static_cast<gpio_num_t>(TOUCH_INPUT_PIN), RTC_GPIO_MODE_INPUT_ONLY);
  // TTP223 drives GPIO13 push-pull. Do not bias the line from the ESP while asleep.
  rtc_gpio_pullup_dis(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  wakeError=esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(TOUCH_INPUT_PIN),1);
#elif CONFIG_IDF_TARGET_ESP32C3
  wakeError=esp_deep_sleep_enable_gpio_wakeup(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error Unsupported Synap sleep target
#endif
  if (wakeError!=ESP_OK) {
    Serial.printf("[POWER] failed to arm touch wake err=%d\n",int(wakeError));
    return false;
  }
  synapLastSleepStage=SLEEP_STAGE_WAKE_ARMED;
  return true;
}

void armTouchWakeAndSleep() {
  synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
  sleepPending=true;
  if (!armTouchWakeSource()) {
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    Serial.println("[POWER] fail-closed wake arm failed; rebooting with sleep lock retained");
    delay(250);
    ESP.restart();
    return;
  }
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] deep sleep now request=%u gpio=%u\n",
    unsigned(synapSleepRequestCounter),unsigned(digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL));
  esp_deep_sleep_start();
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting fail-closed");
  delay(250);
  ESP.restart();
}`,'fail-closed wake source');

  out=replaceFunction(out,'bool confirmTouchWakeTripleTap()',`bool confirmTouchWakeTripleTap() {
  const bool durableLock=readDurableSleepLock();
  const bool sleepResume=durableLock || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) || bootSleepWasLocked;
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bootWakeCause=cause;
  synapLastWakeCause=static_cast<uint8_t>(cause);
  bootSleepWasLocked=sleepResume;
  Serial.printf("[POWER] wake cause=%u sleepLock=%u rtcMarker=%u stage=%u request=%u\n",
    unsigned(cause),durableLock?1u:0u,
    synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER?1u:0u,
    unsigned(synapLastSleepStage),unsigned(synapSleepRequestCounter));
  if (!sleepResume) return true;

  bool touchWake=false;
#if CONFIG_IDF_TARGET_ESP32S3
  touchWake=(cause==ESP_SLEEP_WAKEUP_EXT0);
#elif CONFIG_IDF_TARGET_ESP32C3
  touchWake=(cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
  if (!touchWake) {
    // A reset/brownout/watchdog during the shutdown path is not permission to boot BLE.
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);
    armTouchWakeAndSleep();
    return false;
  }

  // The electrical touch wake counts as tap 1. Keep both durable and RTC locks set
  // until taps 2 and 3 are validated, so any reset during this window still fails closed.
  synapLastSleepStage=SLEEP_STAGE_WAKE_VALIDATING;
  Serial.println("[TOUCH] deep-sleep wake: tap 1/3; waiting for taps 2 and 3");
  const uint32_t firstPressedAt=millis();
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
    if (uint32_t(millis()-firstPressedAt)>WAKE_TAP_MAX_MS+250u) {
      Serial.println("[TOUCH] wake tap too long; returning to deep sleep");
      delay(30);armTouchWakeAndSleep();return false;
    }
    delay(5);
  }

  uint8_t taps=1;
  const uint32_t windowStarted=millis();
  while (taps<3 && uint32_t(millis()-windowStarted)<WAKE_TRIPLE_WINDOW_MS) {
    const uint32_t waitStarted=millis();
    while (digitalRead(TOUCH_INPUT_PIN)!=TOUCH_ACTIVE_LEVEL) {
      if (uint32_t(millis()-waitStarted)>WAKE_TAP_GAP_MS ||
          uint32_t(millis()-windowStarted)>=WAKE_TRIPLE_WINDOW_MS) {
        Serial.printf("[TOUCH] wake sequence incomplete at %u/3; returning to deep sleep\n",unsigned(taps));
        delay(30);armTouchWakeAndSleep();return false;
      }
      delay(5);
    }

    const uint32_t pressedAt=millis();
    while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL &&
        uint32_t(millis()-pressedAt)<=WAKE_TAP_MAX_MS) delay(5);
    const uint32_t held=uint32_t(millis()-pressedAt);
    if (held<WAKE_TAP_MIN_MS || held>WAKE_TAP_MAX_MS ||
        digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      Serial.println("[TOUCH] invalid wake tap; returning to deep sleep");
      while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) delay(5);
      delay(30);armTouchWakeAndSleep();return false;
    }
    ++taps;
    Serial.printf("[TOUCH] wake tap %u/3\n",unsigned(taps));
  }

  if (taps!=3) {
    delay(30);armTouchWakeAndSleep();return false;
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();return false;
  }
  synapDeepSleepMarker=0;
  sleepPending=false;
  synapLastSleepStage=SLEEP_STAGE_WAKE_CONFIRMED;
  Serial.println("[TOUCH] triple tap wake confirmed; sleep lock cleared; continuing normal boot");
  touchRawState=false;touchStableState=false;touchPressedAt=0;touchFirstTapAt=0;
  touchChangedAt=millis();
  wakeRecordIntent=false;
  return true;
}`,'durable triple-tap wake gate');

  out=replaceFunction(out,'void enterDeepSleep(const char* reason)',`void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load() || sleepPending) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;

  const uint32_t initialReleaseAt=millis();
  while (uint32_t(millis()-initialReleaseAt)<300u) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      Serial.println("[TOUCH] sleep cancelled: touch line was not released");
      return;
    }
    delay(10);
  }

  remoteStandby=false;
  wakeRecordIntent=false;
  sleepPending=true;
  ++synapSleepRequestCounter;
  synapLastSleepStage=SLEEP_STAGE_REQUESTED;
  synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;

  // The durable lock is committed before any operation that could reset or drop BLE.
  if (!writeDurableSleepLock(true)) {
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    synapDeepSleepMarker=0;
    sleepPending=false;
    Serial.println("[POWER] durable sleep lock write failed; staying awake");
    return;
  }
  synapLastSleepStage=SLEEP_STAGE_LOCKED;
  Serial.printf("[POWER] sleep lock committed request=%u reason=%s\n",
    unsigned(synapSleepRequestCounter),reason?reason:"idle");

#if USE_REAL_I2S_MIC
  if (microphoneReady) stopMicrophone();
#endif
  applyCpuPowerProfile(false);

  // Recheck the TTP223 immediately before arming the level wake source.
  const uint32_t finalReleaseAt=millis();
  while (uint32_t(millis()-finalReleaseAt)<300u) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      writeDurableSleepLock(false);
      synapDeepSleepMarker=0;
      synapLastSleepStage=SLEEP_STAGE_ABORTED;
      sleepPending=false;
      Serial.println("[TOUCH] sleep cancelled: GPIO13 changed before wake arm");
      return;
    }
    delay(10);
  }
  synapLastGpioBeforeSleep=static_cast<uint8_t>(digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL);
  synapLastSleepStage=SLEEP_STAGE_GPIO_RELEASED;

  if (!armTouchWakeSource()) {
    writeDurableSleepLock(false);
    synapDeepSleepMarker=0;
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    sleepPending=false;
    Serial.println("[POWER] wake source could not be armed; sleep cancelled");
    return;
  }

  // Tell the app only after the durable lock and wake source are ready. No BLE deinit
  // is performed here; deep sleep itself tears down the radio without a reset window.
  Serial.println("[POWER] sleep pending; BLE commands locked; wake source armed");
  publishPowerEvent(POWER_STATE_DEEP_SLEEP);
  if (deviceConnected.load()) delay(90);

  // Any edge after wake arming is handled fail-closed by the boot gate.
  statusLed.clear();statusLed.show();
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] entering deep sleep request=%u battery=%umV\n",
    unsigned(synapSleepRequestCounter),unsigned(batteryMillivolts));
  esp_deep_sleep_start();

  // Deep sleep should not return. If it does, retain fail-closed semantics.
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting with sleep lock retained");
  delay(250);
  ESP.restart();
}`,'fail-closed deep sleep transition');

  out=replaceOnce(out,
`  Serial.begin(115200);
  if (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) delay(20);
  else delay(400);
  bootResetReason=esp_reset_reason();
#if CONFIG_IDF_TARGET_ESP32S3
  if (esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT0 ||
      synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  }
#endif
  pinMode(TOUCH_INPUT_PIN, INPUT);`,
`  Serial.begin(115200);
  bootResetReason=esp_reset_reason();
  bootWakeCause=esp_sleep_get_wakeup_cause();
  bootSleepWasLocked=readDurableSleepLock() || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER);
  if (bootSleepWasLocked) delay(20);
  else delay(400);
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0 || bootSleepWasLocked) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  }
#endif
  pinMode(TOUCH_INPUT_PIN, INPUT);`,
  'boot sleep lock before BLE');

  out=replaceOnce(out,
`  if (otaBusy()) flags|=0x08;
  value[2]=flags;value[3]=static_cast<uint8_t>(bootResetReason);`,
`  if (otaBusy()) flags|=0x08;
  if (bootSleepWasLocked) flags|=0x10;
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0) flags|=0x20;
#elif CONFIG_IDF_TARGET_ESP32C3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_GPIO) flags|=0x20;
#endif
  value[2]=flags;value[3]=static_cast<uint8_t>(bootResetReason);`,
  'diagnostic sleep-lock flags');

  if(out.includes('BLEDevice::deinit(true)'))throw new Error('Fail-closed power contract still contains BLEDevice::deinit(true)');
  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-power-failclosed.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap fail-closed power state: durable sleep lock, no BLE teardown race, triple-tap unlock');
}
module.exports={patch,replaceOnce,replaceFunction};