bool armTouchWakeSource() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t wakeError=ESP_FAIL;
#if CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_init(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  rtc_gpio_set_direction(static_cast<gpio_num_t>(TOUCH_INPUT_PIN), RTC_GPIO_MODE_INPUT_ONLY);
  // TTP223 drives the configured touch GPIO push-pull. Do not bias the line from the ESP while asleep.
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
}

bool confirmTouchWakeGesture() {
  const bool durableLock=readDurableSleepLock();
  const bool sleepResume=durableLock || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) || bootSleepWasLocked;
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bootWakeCause=cause;
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
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // Both boards confirm a deliberate hold after hardware wake. The TTP223
  // high level performs the hardware wake; firmware then confirms the hold.
  constexpr uint16_t TOUCH_WAKE_HOLD_MS = 4000;
  synapLastSleepStage=SLEEP_STAGE_WAKE_VALIDATING;
  const uint32_t pressedAt=millis();
  Serial.println("[TOUCH] wake touch detected; hold to power on");
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL &&
      uint32_t(millis()-pressedAt)<TOUCH_WAKE_HOLD_MS) delay(5);

  if (uint32_t(millis()-pressedAt)<TOUCH_WAKE_HOLD_MS) {
    Serial.println("[TOUCH] wake press too short; returning to deep sleep");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // Do not continue into normal touch handling until the wake press is released.
  uint32_t releasedAt=millis();
  while (uint32_t(millis()-releasedAt)<TOUCH_DEBOUNCE_MS) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) releasedAt=millis();
    delay(5);
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();return false;
  }
  synapDeepSleepMarker=0;
  sleepPending=false;
  synapLastSleepStage=SLEEP_STAGE_WAKE_CONFIRMED;
  Serial.println("[TOUCH] long-press wake confirmed; sleep lock cleared; continuing normal boot");
  touchRawState=false;touchStableState=false;touchPressedAt=0;
  touchChangedAt=millis();
  return true;
}

void publishPowerEvent(uint8_t powerState) {
  if (!eventCharacteristic || !deviceConnected.load()) return;
  uint8_t value[6] = {POWER_EVENT_MAGIC, POWER_EVENT_VERSION, powerState,
    static_cast<uint8_t>(deviceState),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD & 255),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD >> 8)};
  eventCharacteristic->setValue(value,sizeof(value));
  eventCharacteristic->notify();
}

bool exitRemoteStandby() {
  if (!remoteStandby || sleepPending) return !sleepPending;
  if (otaBusy()) return false;
  remoteStandby=false;
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  configureTransportFromPeerMtu();
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_AWAKE);
  Serial.println("[POWER] remote standby -> awake; microphone remains off until START");
  return true;
}

void enterRemoteStandby() {
  if (sleepPending) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
#if SYNAP_CHAKSHU
  if (mediaBusy()) { updateStatusCharacteristic(true); return; }
#endif
  if (streamingEnabled.load()) stopStreaming();
  remoteStandby=true;
#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_STANDBY);
  statusLed.clear();statusLed.show();
  Serial.println("[POWER] remote standby; BLE available, mic/I2S off");
}

void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load() || sleepPending) return;
#if SYNAP_CHAKSHU
  if (mediaBusy()) return;
#endif
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
  stopMicrophone();
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
      Serial.println("[TOUCH] sleep cancelled: touch input changed before wake arm");
      return;
    }
    delay(10);
  }
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

void pollTouchControl() {
  constexpr uint16_t TOUCH_TAP_MIN_MS = 60;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 500;
  constexpr uint16_t TOUCH_DOUBLE_TAP_GAP_MS = 550;
  constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 4000;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 250;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool standbyAfterStop = false;
  static bool deepSleepAfterStop = false;
  static uint8_t tapCount = 0;
  static uint32_t lastTapAt = 0;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  // An OTA-interrupted press must not become a power gesture when OTA finishes.
  if (otaBusy() || sleepPending) {
    touchPressedAt=0;tapCount=0;lastTapAt=0;
    deepSleepAfterStop=false;standbyAfterStop=false;
  }

  if (deepSleepAfterStop && !streaming && !raw && !otaBusy()) {
    deepSleepAfterStop=false;
    enterDeepSleep("touch-hold-after-stop");
    return;
  }
  if (standbyAfterStop && !streaming && !raw && !otaBusy()) {
    standbyAfterStop=false;
    enterRemoteStandby();
    return;
  }

  if (connected!=lastConnectedState || streaming!=lastStreamingState || standby!=lastStandbyState) {
    lastConnectedState=connected;
    lastStreamingState=streaming;
    lastStandbyState=standby;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    tapCount=0;
    lastTapAt=0;
  }

  // A lone tap intentionally does nothing. Expire it after the double-tap window.
  if (tapCount==1 && lastTapAt && uint32_t(now-lastTapAt)>TOUCH_DOUBLE_TAP_GAP_MS) {
    tapCount=0;
    lastTapAt=0;
  }

  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (otaBusy() || sleepPending || static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;
        tapCount=0;
        lastTapAt=0;
        return;
      }
      touchPressedAt=now;
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held || otaBusy()) {
      tapCount=0;lastTapAt=0;return;
    }

    // Sleep is requested only after release, preventing the level-sensitive
    // wake source from immediately waking again on either board.
    if (held>=TOUCH_SLEEP_HOLD_MS) {
      tapCount=0;lastTapAt=0;
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] long press -> DEEP SLEEP");
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("touch-hold");
      }
      return;
    }

    if (held<TOUCH_TAP_MIN_MS || held>TOUCH_TAP_MAX_MS) {
      tapCount=0;lastTapAt=0;
      return;
    }

    if (!tapCount || !lastTapAt || uint32_t(now-lastTapAt)>TOUCH_DOUBLE_TAP_GAP_MS) {
      tapCount=1;
      lastTapAt=now;
      return;
    }

    // Second valid tap acts immediately; a third tap has no power action.
    tapCount=0;lastTapAt=0;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    if (streamingEnabled.load()) {
      standbyAfterStop=true;
      Serial.println("[TOUCH] double tap -> STOP + POWER SAVER");
      queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
    } else if (deviceConnected.load()) {
      Serial.println(remoteStandby ? "[TOUCH] double tap standby -> START" : "[TOUCH] double tap -> START");
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
    }
  }
}

