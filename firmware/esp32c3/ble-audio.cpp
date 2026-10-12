// Native NimBLE queue acceptance and subscription ownership for C3 PCM audio.
class AudioCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {
    if(peer.getConnHandle()==c3BleHandle.load())c3AudioSubscribed=(flags&1)!=0;
  }
};
bool c3SendAudio(const uint8_t* bytes,size_t length) {
  const uint16_t handle=c3BleHandle.load();
  if(!deviceConnected.load() || !c3AudioSubscribed.load() ||
     handle==BLE_HS_CONN_HANDLE_NONE)return false;
  constexpr int reserve=4;
  if(os_msys_num_free()<=reserve) {
    lastNotifyStatus=4;lastNotifyError=BLE_HS_ENOMEM;++notifyRejected;return false;
  }
  os_mbuf* packet=ble_hs_mbuf_from_flat(bytes,length);
  if(packet && os_msys_num_free()<reserve) {
    os_mbuf_free_chain(packet);lastNotifyStatus=4;
    lastNotifyError=BLE_HS_ENOMEM;++notifyRejected;return false;
  }
  const int result=packet?ble_gattc_notify_custom(handle,audioCharacteristic->getHandle(),packet):BLE_HS_ENOMEM;
  if(result) {lastNotifyStatus=4;lastNotifyError=uint32_t(result);++notifyRejected;return false;}
  return true;
}
