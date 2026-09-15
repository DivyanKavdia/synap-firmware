class AudioCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {
    if(peer.getConnHandle()==chakshuConnectionHandle.load())chakshuAudioSubscribed=(flags&1)!=0;
  }
};

// Only the transmitter task owns this cursor. A congested fragment is retried
// in place; restarting at zero can starve the tail of every PCM frame.
class ChakshuAudioProgress {
  uint32_t generation=0, connection=0, replay=0;
  uint16_t sequence=0, payload=0;
  uint8_t chunks=0, next=0;
  bool pcm=false, valid=false;
 public:
  uint8_t begin(uint32_t g,uint32_t c,uint32_t r,uint16_t s,uint8_t n,uint16_t p,bool raw) {
    if (!valid || generation!=g || connection!=c || replay!=r || sequence!=s || chunks!=n || payload!=p || pcm!=raw) {
      generation=g;connection=c;replay=r;sequence=s;chunks=n;payload=p;pcm=raw;next=0;valid=true;
    }
    return next;
  }
  void accept(uint8_t index) { next=index+1; }
  void reset() { valid=false; }
} chakshuAudioProgress;

bool sendChakshuAudio(const uint8_t* bytes,size_t length) {
  const uint16_t connection=chakshuConnectionHandle.load();
  if(!deviceConnected.load()||!chakshuAudioSubscribed.load()||connection==BLE_HS_CONN_HANDLE_NONE)return false;
  // Check the actual allocation below: packet headers and the host's block
  // sizes determine its cost. A guessed per-byte budget can reject every PCM
  // fragment even when the shared pool has enough room for audio and control.
  constexpr int controlReserve=4;
  if(os_msys_num_free()<=controlReserve) {
    lastNotifyStatus=4;lastNotifyError=BLE_HS_ENOMEM;++notifyRejected;
    return false;
  }
  // Queue acceptance is synchronous. TX callbacks are asynchronous and must not
  // decide whether a later fragment was accepted. The host consumes a non-null
  // mbuf on both success and failure; a null allocation must never become a read.
  os_mbuf* packet=ble_hs_mbuf_from_flat(bytes,length);
  if(packet && os_msys_num_free()<controlReserve) {
    os_mbuf_free_chain(packet);
    lastNotifyStatus=4;lastNotifyError=BLE_HS_ENOMEM;++notifyRejected;
    return false;
  }
  const int result=packet?ble_gattc_notify_custom(connection,audioCharacteristic->getHandle(),packet):BLE_HS_ENOMEM;
  if(result!=0) {
    lastNotifyStatus=4;lastNotifyError=uint32_t(result);++notifyRejected;
    return false;
  }
  return true;
}
