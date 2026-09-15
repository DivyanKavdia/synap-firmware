void updateStatusLed(bool force) {
  const uint32_t now = millis();
  uint8_t r=0,g=0,b=0;
  if (otaBusy()) {
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
  } else if (remoteStandby) {
    // Standby stays dark; battery telemetry remains available over BLE.
  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {
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

