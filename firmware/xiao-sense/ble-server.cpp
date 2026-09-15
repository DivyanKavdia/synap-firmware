class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server,NimBLEConnInfo& peer) override {
    if(deviceConnected.load()) { server->disconnect(peer.getConnHandle());return; }
    chakshuConnectionHandle=peer.getConnHandle();
    chakshuAudioSubscribed=false;
    ChakshuLink::connectedAt=millis();ChakshuLink::statusSeen=false;
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled=false;
    deviceConnected=true;connectionEventPending=true;
    // Keep the central's connection parameters. The pinned host requests maximum
    // data length immediately AFTER this callback; requesting it here duplicates
    // that procedure. MTU negotiation and discovery need no extra link tuning.
  }
  void onDisconnect(NimBLEServer*,NimBLEConnInfo& peer,int reason) override {
    if(peer.getConnHandle()!=chakshuConnectionHandle.load())return;
    ChakshuLink::lastDurationMs=uint32_t(millis()-ChakshuLink::connectedAt.load());
    ChakshuLink::lastInterval=peer.getConnInterval();
    ChakshuLink::lastLatency=peer.getConnLatency();
    ChakshuLink::lastTimeout=peer.getConnTimeout();
    ChakshuLink::lastStage=ChakshuLink::stage(true,streamingEnabled.load()&&!recoveryWaiting.load(),chakshuAudioSubscribed.load());
    deviceConnected=false;chakshuAudioSubscribed=false;
    chakshuConnectionHandle=BLE_HS_CONN_HANDLE_NONE;
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);
    if(recoveryEnabled.load()&&streamingEnabled.load()) {
      recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    }else streamingEnabled=false;
    ++connectionGeneration;connectionEventPending=true;
  }
};
