void updateStatusLed(bool force) {
  const uint32_t now=millis();
  bool on=false;
  if (otaBusy()) {
    const uint32_t phase=now%1400u;
    on=phase<55u || (phase>=180u && phase<235u);
  } else if (!sleepPending) {
    if (deviceState==DeviceState::STREAMING) on=now%1000u<100u;
    // Connected standby keeps a sparse heartbeat without changing its power policy.
    else if (deviceState==DeviceState::DISCONNECTED) on=now%6000u<100u;
    else if (deviceState==DeviceState::CONNECTED_IDLE) {
      const uint32_t phase=now%3000u;
      on=phase<80u || (phase>=240u && phase<320u);
    }
    else on=now%1200u<70u;
  }
  const uint32_t pattern=on?1u:0u;
  if (!force && pattern==lastLedPattern) return;
  lastLedPattern=pattern;
  digitalWrite(RGB_LED_PIN,on?LOW:HIGH); // Onboard blue LED is active-low.
}
