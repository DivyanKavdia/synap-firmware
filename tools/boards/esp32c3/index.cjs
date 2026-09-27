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

  // ESP32-C3 + iOS/Bluefy link hardening. Do not renegotiate connection
  // parameters from the peripheral inside the connect callback; CoreBluetooth
  // must be allowed to complete the native link/GATT cache transition first.
  const connectBefore=`#if defined(CONFIG_BLUEDROID_ENABLED)
  // Arduino 3.3.5 calls BOTH overloads; the common overload owns state changes.
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    if(param)server->updateConnParams(param->connect.remote_bda, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }
  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    (void)server;
    if(param)lastDisconnectReason=static_cast<uint16_t>(param->disconnect.reason);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {
    if(desc)server->updateConnParams(desc->conn_handle, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }
  // This Arduino NimBLE callback omits the reason; retain 0xFFFF (unavailable).
#endif`;
  const connectAfter=`#if defined(CONFIG_BLUEDROID_ENABLED)
  // Arduino 3.3.5 calls BOTH overloads; the common overload owns state changes.
  // C3: leave initial connection parameters to CoreBluetooth. Immediate
  // peripheral-initiated renegotiation can race Bluefy/iOS link establishment.
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    (void)server;
    (void)param;
  }
  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    (void)server;
    if(param)lastDisconnectReason=static_cast<uint16_t>(param->disconnect.reason);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  // C3: leave initial connection parameters to CoreBluetooth.
  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {
    (void)server;
    (void)desc;
  }
  // This Arduino NimBLE callback omits the reason; retain 0xFFFF (unavailable).
#endif`;
  out=replaceOnce(out,connectBefore,connectAfter,'C3 Bluefy connect timing');

  // Give the C3 a deterministic random-static BLE address derived from its
  // factory eFuse MAC. This is a one-time identity migration from the previous
  // public address, so iOS cannot reuse stale GATT handles after the Synap
  // service table changed. The derived address is stable across later boots.
  const initBefore=`void initializeBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(REQUESTED_MTU);`;
  const initAfter=`void initializeBLE() {
  BLEDevice::init(DEVICE_NAME);
  uint8_t c3BleAddress[6] = {};
  const bool c3BleAddressReady = esp_efuse_mac_get_default(c3BleAddress) == ESP_OK;
  if (c3BleAddressReady) {
    c3BleAddress[0] = uint8_t((c3BleAddress[0] & 0x3Fu) | 0xC0u);
#if defined(CONFIG_NIMBLE_ENABLED)
    BLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    BLEDevice::setOwnAddr(c3BleAddress);
#endif
  }
  BLEDevice::setMTU(REQUESTED_MTU);`;
  out=replaceOnce(out,initBefore,initAfter,'C3 stable random BLE identity');

  // Keep the short name and 128-bit Synap service UUID in the primary
  // advertisement. It fits in the 31-byte legacy packet and avoids depending
  // on an active scan response before Bluefy selects the peripheral.
  const advertisingBefore=`  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);`;
  const advertisingAfter=`  BLEAdvertising* advertising=BLEDevice::getAdvertising();
#if defined(CONFIG_BLUEDROID_ENABLED)
  if (c3BleAddressReady) advertising->setDeviceAddress(c3BleAddress, BLE_ADDR_TYPE_RANDOM);
#elif defined(CONFIG_NIMBLE_ENABLED)
  advertising->setName(DEVICE_NAME);
#endif
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(false);`;
  out=replaceOnce(out,advertisingBefore,advertisingAfter,'C3 primary BLE advertisement');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`"SYNAP-FW:${target.id}:" SYNAP_VERSION`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  return out;
}

module.exports={materializeC3};
