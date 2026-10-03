'use strict';
const {PRIMARY_TARGET}=require('../../targets.cjs');
const {replaceOnce}=require('../../target-source.cjs');
function materializeC3(source,target){
  let out=replaceOnce(source,'p[21]!=9 || p[22]!=0',`p[21]!=${target.chip} || p[22]!=0`,'ESP image chip ID');
  out=replaceOnce(out,'analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);',
    `analogSetPinAttenuation(BATTERY_ADC_PIN, ${target.hardware.batteryAttenuation});`,'ADC input range');

  const taskBefore=`  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {`;
  const taskAfter=`  // ESP32-C3 has one core; retain task priorities and stack sizes without pinning.
  if (xTaskCreate(controlTask, "control", 8192, nullptr, 3, nullptr) != pdPASS ||
      xTaskCreate(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle) != pdPASS ||
      xTaskCreate(transmitterTask, "transmit", 8192, nullptr, 2, nullptr) != pdPASS) {`;
  out=replaceOnce(out,taskBefore,taskAfter,'single-core task creation');
  // Offline SD audio owns the C3 CPU while the shared S3/Chakshu source
  // retains its independent power-profile materialization anchor.
  out=replaceOnce(out,'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());',
    'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu() || odysseyRecording.load());',
    'C3 SD capture CPU profile');

  // C3/iOS/Bluefy: do not renegotiate connection parameters inside the native
  // connect callback.  The central already receives our preferred range in the
  // advertisement; allowing the initial link to settle avoids an early native
  // disconnect before Web Bluetooth can discover the Synap GATT service.
  const bluedroidConnectBefore=`  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    if(param)server->updateConnParams(param->connect.remote_bda, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }`;
  const bluedroidConnectAfter=`  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    // C3: let iOS/Bluefy finish the native BLE link before any optional tuning.
    (void)server;
    (void)param;
  }`;
  out=replaceOnce(out,bluedroidConnectBefore,bluedroidConnectAfter,'C3 Bluedroid connect parameters');

  const nimbleConnectBefore=`  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {
    if(desc)server->updateConnParams(desc->conn_handle, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }`;
  const nimbleConnectAfter=`  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {
    // C3: let iOS/Bluefy finish the native BLE link before any optional tuning.
    (void)server;
    (void)desc;
  }`;
  out=replaceOnce(out,nimbleConnectBefore,nimbleConnectAfter,'C3 NimBLE connect parameters');

  // Put both the local name and the Synap 128-bit service UUID in the primary
  // advertisement.  Bluefy/iOS can otherwise surface the C3 as "unnamed"
  // because the library normally places the name only in the scan response.
  const advertisingBefore=`  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(BLE_MIN_INTERVAL);
  advertising->setMaxPreferred(BLE_MAX_INTERVAL);
  advertising->start();`;
  const advertisingAfter=`  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  BLEAdvertisementData primaryAdvertisement;
  primaryAdvertisement.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  primaryAdvertisement.setName(DEVICE_NAME);
  primaryAdvertisement.setCompleteServices(BLEUUID(SERVICE_UUID));
  if (!advertising->setAdvertisementData(primaryAdvertisement))
    Serial.println("[BLE] primary advertisement configuration failed");
  BLEAdvertisementData scanResponse;
  scanResponse.addTxPower();
  scanResponse.setPreferredParams(BLE_MIN_INTERVAL, BLE_MAX_INTERVAL);
  if (!advertising->setScanResponseData(scanResponse))
    Serial.println("[BLE] scan response configuration failed");
  advertising->setScanResponse(true);
  advertising->setMinPreferred(BLE_MIN_INTERVAL);
  advertising->setMaxPreferred(BLE_MAX_INTERVAL);
  if (advertising->start()) Serial.printf("[BLE] advertising name=%s explicit=1\\n", DEVICE_NAME);
  else Serial.println("[BLE] advertising start failed");`;
  out=replaceOnce(out,advertisingBefore,advertisingAfter,'C3 explicit primary advertisement');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`"SYNAP-FW:${target.id}:" SYNAP_VERSION`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  if(!out.includes('primaryAdvertisement.setName(DEVICE_NAME)'))throw Error('C3 BLE local name is not in the primary advertisement');
  if(!out.includes('primaryAdvertisement.setCompleteServices(BLEUUID(SERVICE_UUID))'))throw Error('C3 BLE service UUID is not in the primary advertisement');
  if(out.includes('if(desc)server->updateConnParams(desc->conn_handle'))throw Error('C3 still renegotiates NimBLE connection parameters during connect');
  if(out.includes('if(param)server->updateConnParams(param->connect.remote_bda'))throw Error('C3 still renegotiates Bluedroid connection parameters during connect');
  return out;
}

module.exports={materializeC3};
