// Odyssey C3 native NimBLE link, preserving shipped GATT/service semantics.
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server,NimBLEConnInfo& peer) override {
    if(deviceConnected.load()) { server->disconnect(peer.getConnHandle());return; }
    c3BleHandle=peer.getConnHandle();c3AudioSubscribed=false;
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled=false;
    deviceConnected=true;connectedLedAt=millis();connectionEventPending=true;
  }
  void onDisconnect(NimBLEServer*,NimBLEConnInfo& peer,int reason) override {
    if(peer.getConnHandle()!=c3BleHandle.load())return;
    deviceConnected=false;c3AudioSubscribed=false;c3BleHandle=BLE_HS_CONN_HANDLE_NONE;
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);
    if(recoveryEnabled.load() && streamingEnabled.load()) {
      recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    } else streamingEnabled=false;
    ++connectionGeneration;connectionEventPending=true;
  }
};
class C3ControlCharacteristic : public NimBLECharacteristic {
 public:
  C3ControlCharacteristic() : NimBLECharacteristic(CONTROL_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY, 16) {}
 private:
  // Native NimBLE default writeEvent overwrites status with incoming command
  // bytes. Consume the GATT write directly without overwriting read/notify status.
  void writeEvent(const uint8_t* bytes,uint16_t length,NimBLEConnInfo& peer) override {
    if(!deviceConnected.load() || peer.getConnHandle()!=c3BleHandle.load())return;
    const uint8_t command=length==2 && bytes?bytes[0]:0xFF;
    const uint8_t version=length==2 && bytes?bytes[1]:0;
    queueEvent(EventType::COMMAND,command,version,streamGeneration.load());
  }
};
