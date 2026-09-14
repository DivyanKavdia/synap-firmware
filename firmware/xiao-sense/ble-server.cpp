class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server,NimBLEConnInfo& peer) override {
    if(deviceConnected.load()) { server->disconnect(peer.getConnHandle());return; }
    chakshuConnectionHandle=peer.getConnHandle();
    chakshuAudioSubscribed=false;
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled=false;
    deviceConnected=true;connectionEventPending=true;
    server->updateConnParams(peer.getConnHandle(),BLE_MIN_INTERVAL,BLE_MAX_INTERVAL,
      BLE_SLAVE_LATENCY,BLE_SUPERVISION_TIMEOUT);
    server->setDataLen(peer.getConnHandle(),251);
  }
  void onDisconnect(NimBLEServer*,NimBLEConnInfo& peer,int reason) override {
    if(peer.getConnHandle()!=chakshuConnectionHandle.load())return;
    deviceConnected=false;chakshuAudioSubscribed=false;
    chakshuConnectionHandle=BLE_HS_CONN_HANDLE_NONE;
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);
    if(recoveryEnabled.load()&&streamingEnabled.load()) {
      recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    }else streamingEnabled=false;
    ++connectionGeneration;connectionEventPending=true;
  }
};
