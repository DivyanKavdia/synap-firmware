void initializeBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(REQUESTED_MTU);
  bleServer=BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
#if defined(CONFIG_NIMBLE_ENABLED)
  bleServer->advertiseOnDisconnect(true);
#endif
  // Audio/control + device ID + OTA/status/build identity + diagnostics exceed
  // Bluedroid's default service reservation. NimBLE accepts this overload as well.
  BLEService* service=bleServer->createService(BLEUUID(SERVICE_UUID),64);
  audioCharacteristic=service->createCharacteristic(AUDIO_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  audioCharacteristic->setCallbacks(new AudioCallbacks());
  controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  controlCharacteristic->setCallbacks(new ControlCallbacks());
  // Battery and power events have a dedicated notification channel.
  eventCharacteristic=service->createCharacteristic(EVENT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  audioCccd=new BLE2902();
  audioCharacteristic->addDescriptor(audioCccd);
  controlCharacteristic->addDescriptor(new BLE2902());
  eventCharacteristic->addDescriptor(new BLE2902());
#endif
  // NimBLE creates CCCDs itself. BLE2902::getNotifications() is NOT a
  // subscription test under NimBLE; do not use it to gate START.
  auto* deviceIdentity = service->createCharacteristic(DEVICE_ID_UUID, BLECharacteristic::PROPERTY_READ);
  deviceIdentity->setValue(synapDeviceId);
  diagnosticsCharacteristic=service->createCharacteristic(DIAGNOSTICS_UUID,BLECharacteristic::PROPERTY_READ);
  diagnosticsCharacteristic->setCallbacks(new DiagnosticsCallbacks());
  recoveryCharacteristic=service->createCharacteristic(RECOVERY_CHAR_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  recoveryCharacteristic->addDescriptor(new BLE2902());
#endif
  recoveryCharacteristic->setCallbacks(new RecoveryCallbacks());
  updateDiagnosticsCharacteristic();
  updateStatusCharacteristic(false);
  otaInitialize(service);
  initializeModuleCapabilities(service);
#if SYNAP_CHAKSHU
  ChakshuMedia::ble(service);
  ChakshuTransfer::ble(service);
#elif CONFIG_IDF_TARGET_ESP32C3
  OdysseyTransfer::ble(service);
#endif
  service->start();
  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(BLE_MIN_INTERVAL);
  advertising->setMaxPreferred(BLE_MAX_INTERVAL);
  advertising->start();
}
void fatalSetup(const char* message) {
  Serial.println(message);
  setDeviceState(DeviceState::ERROR, ErrorCode::AUDIO_SOURCE_FAILED);
  for (;;) delay(1000);
}
void setup() {
  Serial.begin(115200);
#if USE_REAL_I2S_MIC
  microphoneMutex=xSemaphoreCreateRecursiveMutexStatic(&microphoneMutexStorage);
  if (!microphoneMutex) fatalSetup("[FATAL] microphone lock unavailable");
#endif
  bootResetReason=esp_reset_reason();
  bootWakeCause=esp_sleep_get_wakeup_cause();
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // A physical power-switch ON is a new user power-on request. NVS survives
  // battery removal but must not strand a cold-booted C3 in a former OFF state.
  // Deep-sleep GPIO wakes are ESP_RST_DEEPSLEEP and retain the 4-second hold.
  if (bootResetReason==ESP_RST_POWERON) {
    const bool staleLock=readDurableSleepLock();
    synapDeepSleepMarker=0;
    if (staleLock && !writeDurableSleepLock(false))
      Serial.println("[POWER] cold boot: could not clear old NVS sleep lock");
    if (staleLock)
      Serial.println("[POWER] cold power-on overrides retained deep-sleep lock");
  }
#endif
  bootSleepWasLocked=readDurableSleepLock() || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER);
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // Even on a transient NVS write failure, the physical power-on must be
  // allowed to complete. Next cold boot will retry clearing the old lock.
  if (bootResetReason==ESP_RST_POWERON) bootSleepWasLocked=false;
#endif
  if (bootSleepWasLocked) delay(20);
  else delay(400);
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0 || bootSleepWasLocked) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  }
#endif
  pinMode(TOUCH_INPUT_PIN, INPUT);
  touchRawState=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  touchStableState=touchRawState;
  touchChangedAt=millis();
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  // C3 + SD uses a 1 MOhm / 470 kOhm battery divider (up to ~1.34 V
  // at a full 4.2 V cell). C3 6 dB ADC attenuation only covers ~1.3 V
  // and is unsuitable here; retain S3's historically validated 6 dB setting.
#if CONFIG_IDF_TARGET_ESP32C3
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#else
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);
#endif
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  if (!confirmTouchWakeGesture()) return;
  disconnectedAt=millis();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  // A C3 may be fitted with the 1 MOhm/470 kOhm SD divider rather than the
  // original 1 MOhm/1 MOhm divider. Do not publish a misleading x2 reading
  // before the SD hardware identification has completed.
#if !CONFIG_IDF_TARGET_ESP32C3
  sampleBattery(true);
#endif
#if USE_REAL_I2S_MIC
  microphoneValidated=startMicrophone();
  if (microphoneValidated) stopMicrophone();
#endif
  applyCpuPowerProfile(false);
  audioFrameQueue=xQueueCreate(20, sizeof(AudioFrame));
  controlQueue=xQueueCreate(12, sizeof(ControlMessage));
  if (!audioFrameQueue || !controlQueue) fatalSetup("[FATAL] queue allocation failed");
  uint8_t factoryMac[6];
  if (esp_efuse_mac_get_default(factoryMac) != ESP_OK) fatalSetup("[FATAL] device identity unavailable");
  snprintf(synapDeviceId, sizeof(synapDeviceId), "SYNAP-%02X%02X%02X%02X%02X%02X",
    factoryMac[0], factoryMac[1], factoryMac[2], factoryMac[3], factoryMac[4], factoryMac[5]);
  Serial.printf("Synap %u %s reset=%u\n", SYNAP_FIRMWARE_BUILD, synapDeviceId, unsigned(bootResetReason));
#if SYNAP_CHAKSHU
  ChakshuMedia::initialize();
  ChakshuTransfer::initialize();
#elif CONFIG_IDF_TARGET_ESP32C3
  // Reproduce the last independently observed healthy lifecycle (build 1445 /
  // 1481): create the transfer worker first, then perform one mount before BLE.
  // The worker cannot touch storage until BLE submits a request.
  OdysseyWifi::initialize();
  OdysseyTransfer::initialize();
  odysseyInitializeSdCardBeforeBle();
  // The initial C3 reading now uses the appropriate divider when SD mounts.
  // Standard C3 without SD retains its 2:1 measurement.
  sampleBattery(true);
#else
  // Odyssey S3 remains a detection-only target.
  odysseyDetectSdCard();
#endif
  initializeBLE();
  initializeRecovery();
  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {
    fatalSetup("[FATAL] task allocation failed");
  }
}
void loop() {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  static bool bootValidated=false;
  // Leave a newly selected image in PENDING_VERIFY long enough to prove that
  // BLE, queues and (when fitted) the microphone survive early runtime startup.
  if (!bootValidated && millis()>5000 && bleServer && audioFrameQueue && controlQueue
#if USE_REAL_I2S_MIC
      && microphoneValidated
#endif
  ) {
    const esp_err_t result=esp_ota_mark_app_valid_cancel_rollback();
    if (result==ESP_OK || result==ESP_ERR_NOT_FOUND) bootValidated=true;
    Serial.printf("[OTA] delayed boot validation result=%d\n",int(result));
  }
  if (!bootValidated) { delay(20); return; }
#endif
  // Recording, BLE and touch run in their own tasks; this loop only validates boot.
  delay(1000);
}
