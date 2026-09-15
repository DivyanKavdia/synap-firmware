class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server,NimBLEConnInfo& peer) override {
    if(deviceConnected.load()) { server->disconnect(peer.getConnHandle());return; }
    chakshuConnectionHandle=peer.getConnHandle();
    chakshuAudioSubscribed=false;
    ChakshuLink::connectedAt=millis();ChakshuLink::statusSeen=false;
    ChakshuLink::interval=peer.getConnInterval();ChakshuLink::latency=peer.getConnLatency();
    ChakshuLink::timeout=peer.getConnTimeout();
    ChakshuLink::paramRequests=0;ChakshuLink::paramRequestCode=0xFFFF;
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled=false;
    deviceConnected=true;connectionEventPending=true;
    // The pinned host owns DLE; the control task defers any timeout correction.
  }
  void onDisconnect(NimBLEServer*,NimBLEConnInfo& peer,int reason) override {
    if(peer.getConnHandle()!=chakshuConnectionHandle.load())return;
    ChakshuLink::lastDurationMs=uint32_t(millis()-ChakshuLink::connectedAt.load());
    ChakshuLink::lastInterval=peer.getConnInterval();
    ChakshuLink::lastLatency=peer.getConnLatency();
    ChakshuLink::lastTimeout=peer.getConnTimeout();
    ChakshuLink::lastParamRequests=ChakshuLink::paramRequests.load();
    ChakshuLink::lastParamRequestCode=ChakshuLink::paramRequestCode.load();
    ChakshuLink::lastStage=ChakshuLink::stage(true,streamingEnabled.load()&&!recoveryWaiting.load(),chakshuAudioSubscribed.load());
    deviceConnected=false;chakshuAudioSubscribed=false;
    chakshuConnectionHandle=BLE_HS_CONN_HANDLE_NONE;
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=uint16_t(reason);
    if(recoveryEnabled.load()&&streamingEnabled.load()) {
      recoveryWaiting=true;if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    }else streamingEnabled=false;
    ++connectionGeneration;connectionEventPending=true;
  }
  void onConnParamsUpdate(NimBLEConnInfo& peer) override {
    if (!deviceConnected.load() || peer.getConnHandle()!=chakshuConnectionHandle.load()) return;
    ChakshuLink::interval=peer.getConnInterval();ChakshuLink::latency=peer.getConnLatency();
    ChakshuLink::timeout=peer.getConnTimeout();
  }
};
