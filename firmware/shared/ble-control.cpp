void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream) {
  const ControlMessage message = {type, command, version, connectionGeneration.load(), stream};
  if (xQueueSend(controlQueue, &message, 0) != pdTRUE) ++controlDrops;
}
void requestStreamError(ErrorCode error, uint32_t generation) {
  portENTER_CRITICAL(&streamErrorMux);
  if (error != ErrorCode::NONE && streamingEnabled.load() && generation == streamGeneration.load() &&
      (pendingStreamError == ErrorCode::NONE || pendingStreamErrorGeneration != generation)) {
    pendingStreamError = error;
    pendingStreamErrorGeneration = generation;
  }
  portEXIT_CRITICAL(&streamErrorMux);
}
void processStreamError() {
  portENTER_CRITICAL(&streamErrorMux);
  const ErrorCode error = pendingStreamError;
  const uint32_t generation = pendingStreamErrorGeneration;
  pendingStreamError = ErrorCode::NONE;
  portEXIT_CRITICAL(&streamErrorMux);
  // A fault from a completed take cannot terminate its replacement.
  if (error != ErrorCode::NONE && streamingEnabled.load() && generation == streamGeneration.load())
    stopStreaming(error);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled.store(false);
    deviceConnected.store(true);
    connectionEventPending.store(true);
  }
  void onDisconnect(BLEServer* server) override {
    (void)server;
    deviceConnected.store(false);
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=0xFFFF;
    if(recoveryEnabled.load() && streamingEnabled.load()) {
      recoveryWaiting=true; if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    } else streamingEnabled.store(false);
    ++connectionGeneration;
    connectionEventPending.store(true);
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
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
#endif
};
class ControlCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic*) override { updateStatusCharacteristic(false); }
#if defined(CONFIG_NIMBLE_ENABLED)
  // Installed by tools/patch-arduino-ble.cjs. Never parse the mutable status
  // characteristic: battery/status publication can replace it during a write.
  void onWriteValue(BLECharacteristic*, ble_gap_conn_desc*, const uint8_t* data, size_t length) override {
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
#elif defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(BLECharacteristic*, esp_ble_gatts_cb_param_t* param) override {
    // Protocol commands are exactly two bytes, never prepared/long writes.
    if (!param || param->write.is_prep || param->write.len != 2) return;
    queueEvent(EventType::COMMAND, param->write.value[0], param->write.value[1], streamGeneration.load());
  }
#else
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    // Defer work so synchronous GATT writes return promptly.
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
#endif
};
class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* characteristic, Status status, uint32_t code) override {
    (void)characteristic;
    if (status != SUCCESS_NOTIFY) {
      lastNotifyStatus=static_cast<uint16_t>(status);lastNotifyError=code;
      ++notifyRejected;
    }
  }
};
class DiagnosticsCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    (void)characteristic;
    updateDiagnosticsCharacteristic();
  }
};

void processCommand(uint8_t command, uint8_t version) {
#if SYNAP_CHAKSHU
  if (command==CMD_STANDBY) { publishPowerEvent(POWER_STATE_AWAKE);updateStatusCharacteristic(true);return; }
#endif
  if (sleepPending) return;
  if (!deviceConnected.load()) { if(command==CMD_STOP && streamingEnabled.load())stopStreaming(); return; }
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  switch (command) {
    case CMD_START:
      if (remoteStandby && !exitRemoteStandby()) break;
      startStreaming(version);
      if (streamingEnabled.load()) {
        publishPowerEvent(POWER_STATE_AWAKE);
      }
      break;
    case CMD_STOP:
      if (remoteStandby) updateStatusCharacteristic(true);
      else if(recoveryEnabled.load() && streamingEnabled.load() && !recoveryWaiting.load())finishBufferedRecording();
      else stopStreaming();
      break;
    case CMD_GET_STATUS:
      if (remoteStandby) {
        // Standby remains CONNECTED_IDLE on protocol v2.
        setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
      } else if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      sampleBattery(true);
      break;
    case CMD_STANDBY:
      if (!streamingEnabled.load()) enterRemoteStandby();
      else updateStatusCharacteristic(true);
      break;
    case CMD_WAKE:
      exitRemoteStandby();
      break;
    default:
      stopStreaming(ErrorCode::BAD_COMMAND);
      break;
  }
}
void reconcileConnection() {
  // Link transitions cannot be lost when the command queue is full. A newer
  // transition during this work leaves the flag set for the next control tick.
  if (!connectionEventPending.exchange(false)) return;
  if (deviceConnected.load()) {
    restartAdvertising=false;
    disconnectedAt=0;
    peerMtu=23; attValueCapacity=20; chunksPerFrame=0; audioPayloadBytes=0;
    if(!(recoveryWaiting.load() && streamingEnabled.load()))stopStreaming();
    publishPowerEvent(remoteStandby ? POWER_STATE_STANDBY : POWER_STATE_AWAKE);
    sampleBattery(true);
  } else {
    if(!(recoveryWaiting.load() && streamingEnabled.load()))stopStreaming();
    disconnectedAt=millis();
    restartAdvertising=true;
    Serial.printf("[BLE] disconnected count=%lu reason=0x%04X uptime=%lus heap=%u rejected=%lu recovery=%u\n",
      static_cast<unsigned long>(linkDisconnects.load()),unsigned(lastDisconnectReason.load()),
      static_cast<unsigned long>(millis()/1000u),unsigned(ESP.getFreeHeap()),
      static_cast<unsigned long>(notifyRejected.load()),unsigned(recoveryWaiting.load()));
  }
}

void controlTask(void* parameter) {
  (void)parameter;
  ControlMessage message;
  for (;;) {
    const bool received=xQueueReceive(controlQueue, &message, pdMS_TO_TICKS(10)) == pdTRUE;
    reconcileConnection();
    processRecoveryRequest();
    if(recoveryWaiting.load() && uint32_t(millis()-recoveryWaitingAt.load())>=60000u)stopStreaming();
    if (received && message.connection == connectionGeneration.load()) {
      switch (message.type) {
        case EventType::COMMAND:
          processCommand(message.command, message.version);
          break;
        case EventType::STREAM_ERROR:
          if (streamingEnabled.load() && message.stream == streamGeneration.load()) {
            stopStreaming(static_cast<ErrorCode>(message.command));
          }
          break;
      }
    }
    processStreamError();
    // A restored link cannot transmit until RESUME binds it to the app journal.
    if(recoveryFinishing.load() && deviceConnected.load() && !recoveryWaiting.load() && (!recoveryCanSend() && !transmitterActive.load() && uint32_t(millis()-recoveryFinishAt)>150u))stopStreaming();
    if(recoveryFinishing.load() && uint32_t(millis()-recoveryFinishAt)>35000u)stopStreaming(ErrorCode::TRANSPORT_CHANGED);
    if (deviceConnected.load() && streamingEnabled.load() && !recoveryWaiting.load()) {
      uint16_t liveMtu=bleServer->getPeerMTU(bleServer->getConnId());
      if (liveMtu<23) liveMtu=23;
      if (liveMtu!=peerMtu) {
        const uint16_t liveCapacity=liveMtu-3;
        if (liveCapacity < uint16_t(AUDIO_HEADER_BYTES+audioPayloadBytes.load())) {
          stopStreaming(ErrorCode::TRANSPORT_CHANGED);
        } else {
          peerMtu=liveMtu;attValueCapacity=liveCapacity;
          updateStatusCharacteristic(true);
        }
      }
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    if (restartAdvertising && !deviceConnected.load() && millis()-disconnectedAt > 250) {
      restartAdvertising=false;
      bleServer->startAdvertising();
    }
#endif
    pollTouchControl();
    otaTick();
#if SYNAP_CHAKSHU
    ChakshuMedia::tick();
#endif
    powerTick();
    applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());
    updateStatusLed();
  }
}
