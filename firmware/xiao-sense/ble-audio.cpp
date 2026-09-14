class AudioCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {
    if(peer.getConnHandle()==chakshuConnectionHandle.load())chakshuAudioSubscribed=(flags&1)!=0;
  }
};

bool sendChakshuAudio(const uint8_t* bytes,size_t length) {
  const uint16_t connection=chakshuConnectionHandle.load();
  if(!deviceConnected.load()||!chakshuAudioSubscribed.load()||connection==BLE_HS_CONN_HANDLE_NONE)return false;
  // Queue acceptance is synchronous. TX callbacks are asynchronous and must not
  // decide whether a later fragment was accepted. The host consumes a non-null
  // mbuf on both success and failure; a null allocation must never become a read.
  os_mbuf* packet=ble_hs_mbuf_from_flat(bytes,length);
  const int result=packet?ble_gattc_notify_custom(connection,audioCharacteristic->getHandle(),packet):BLE_HS_ENOMEM;
  if(result!=0) {
    lastNotifyStatus=4;lastNotifyError=uint32_t(result);++notifyRejected;
    return false;
  }
  return true;
}
