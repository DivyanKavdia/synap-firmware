void pollTouchControl() {
  constexpr uint16_t C3_TAP_MIN_MS = 60;
  constexpr uint16_t C3_TAP_MAX_MS = 500;
  constexpr uint16_t C3_DOUBLE_TAP_GAP_MS = 550;
  constexpr uint16_t C3_SLEEP_HOLD_MS = 4000;
  constexpr uint16_t C3_STATE_LOCKOUT_MS = 250;
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
    enterDeepSleep("c3-touch-hold-after-stop");
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
    touchRearmAt=now+C3_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    tapCount=0;
    lastTapAt=0;
  }

  // A lone tap intentionally does nothing. Expire it after the double-tap window.
  if (tapCount==1 && lastTapAt && uint32_t(now-lastTapAt)>C3_DOUBLE_TAP_GAP_MS) {
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

    // C3-only power gesture. Sleep is requested only after release, preventing
    // the level-sensitive GPIO3 wake source from immediately waking again.
    if (held>=C3_SLEEP_HOLD_MS) {
      tapCount=0;lastTapAt=0;
      touchRearmAt=now+C3_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] C3 long press -> DEEP SLEEP");
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("c3-touch-hold");
      }
      return;
    }

    if (held<C3_TAP_MIN_MS || held>C3_TAP_MAX_MS) {
      tapCount=0;lastTapAt=0;
      return;
    }

    if (!tapCount || !lastTapAt || uint32_t(now-lastTapAt)>C3_DOUBLE_TAP_GAP_MS) {
      tapCount=1;
      lastTapAt=now;
      return;
    }

    // Second valid tap acts immediately; there is no triple-tap ambiguity on C3.
    tapCount=0;lastTapAt=0;
    touchRearmAt=now+C3_STATE_LOCKOUT_MS;
    if (streamingEnabled.load()) {
      standbyAfterStop=true;
      Serial.println("[TOUCH] C3 double tap -> STOP + POWER SAVER");
      queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
    } else if (deviceConnected.load()) {
      Serial.println(remoteStandby ? "[TOUCH] C3 double tap standby -> START" : "[TOUCH] C3 double tap -> START");
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
    }
  }
}
