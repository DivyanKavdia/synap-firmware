void updateStatusLed(bool force) {
  const uint32_t now = millis();
  uint8_t r=0,g=0,b=0;
  if (otaBusy()) {
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  } else if (odysseyRecording.load()) {
    // Match build 1631 exactly: purple tracks the local recording task.
    const uint32_t phase=uint32_t(now-odysseyRecordingStartedAt.load())%1800u;
    if (!odysseyStopRequested.load() && phase<260u) { r=LED_DIM+4; b=LED_DIM+6; }
  } else if (odysseyRecordFaultAt.load() &&
             uint32_t(now-odysseyRecordFaultAt.load())<6000u) {
    // Two red pulses distinguish missing SD / failed capture from active
    // purple recording. Resume normal LED state after six seconds.
    const uint32_t phase=uint32_t(now-odysseyRecordFaultAt.load())%900u;
    if (phase<140u || (phase>=260u && phase<400u)) r=LED_DIM+3;
#endif
  } else if (remoteStandby) {
    // Standby stays dark; battery telemetry remains available over BLE.
  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {
    const uint32_t phase=now%5000u;
    if (phase<40u || (phase>=180u && phase<220u)) r=LED_DIM;
  } else if (deviceState == DeviceState::DISCONNECTED) {
    if (now%5000u<35u) r=LED_DIM;
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {
    // Three visible green acknowledgements confirm the PWA/BLE connection.
    // Connected idle stays dark after the burst to conserve battery.
    const uint32_t connectedAt=connectedLedAt.load();
    const uint32_t elapsed=uint32_t(now-connectedAt);
    if (connectedAt && elapsed<1500u && elapsed%500u<180u) g=LED_DIM+5;
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

