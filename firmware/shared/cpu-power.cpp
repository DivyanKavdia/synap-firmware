void setDeviceState(DeviceState state, ErrorCode error) {
  deviceState = state;
  errorCode = error;
  updateStatusLed(true);
}

void applyCpuPowerProfile(bool active) {
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // The 80 MHz idle profile was also selected while Bluefy was establishing
  // GATT subscriptions or running connected-idle. Avoid changing CPU clocks
  // underneath the single-core NimBLE host and the C3 SD/Wi-Fi tasks.
  // Give a recently dropped link a brief 12 s recovery window at 160 MHz,
  // then return to 80 MHz while advertising to protect battery life.
  const uint32_t lastDisconnect=lastDisconnectAt.load();
  const bool reconnectWindow=lastDisconnect &&
    uint32_t(millis()-lastDisconnect)<12000u;
  active=active || deviceConnected.load() || reconnectWindow ||
    odysseyRecording.load() || OdysseyWifi::busy() || OdysseyTransfer::busy();
#endif
  static uint32_t appliedMHz = 0;
  const uint32_t targetMHz = active ? ACTIVE_CPU_MHZ : IDLE_CPU_MHZ;
  if (appliedMHz == targetMHz) return;
  if (setCpuFrequencyMhz(targetMHz)) {
    appliedMHz = targetMHz;
    Serial.printf("[POWER] cpu=%luMHz mode=%s\n",
      static_cast<unsigned long>(targetMHz),active?"active":"idle");
  } else {
    Serial.printf("[POWER] cpu profile change to %luMHz failed\n",
      static_cast<unsigned long>(targetMHz));
  }
}

