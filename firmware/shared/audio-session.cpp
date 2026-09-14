void updateStatusCharacteristic(bool notify) {
  if (!controlCharacteristic) return;
  uint8_t value[16] = { STATUS_PACKET_MAGIC, PROTOCOL_VERSION,
    static_cast<uint8_t>(deviceState), static_cast<uint8_t>(errorCode) };
  value[4]=peerMtu & 255; value[5]=peerMtu >> 8;
  value[6]=attValueCapacity & 255; value[7]=attValueCapacity >> 8;
  value[8]=chunksPerFrame; value[9]=AUDIO_HEADER_BYTES;
  value[10]=SAMPLE_RATE & 255; value[11]=SAMPLE_RATE >> 8;
  value[12]=SAMPLES_PER_FRAME & 255; value[13]=SAMPLES_PER_FRAME >> 8;
  value[14]=audioPayloadBytes & 255; value[15]=audioPayloadBytes >> 8;
  controlCharacteristic->setValue(value, sizeof(value));
  if (notify && deviceConnected.load()) controlCharacteristic->notify();
}
void updateDiagnosticsCharacteristic() {
  if (!diagnosticsCharacteristic) return;
  uint8_t value[48] = {};
  value[0]=DIAGNOSTICS_MAGIC;value[1]=DIAGNOSTICS_VERSION;
  uint8_t flags=0;
#if USE_REAL_I2S_MIC
  flags|=0x01;
  flags|=0x40; // Captured PCM has no firmware DSP before transport encoding.
#endif
  if (pcmTransport.load()) flags|=0x80; // Uncompressed PCM transport selected.
  if (deviceConnected.load()) flags|=0x02;
  if (streamingEnabled.load()) flags|=0x04;
  // The GATT read callback must not inspect the control task's mutable OTA engine.
  if (otaBusySnapshot.load()) flags|=0x08;
  if (bootSleepWasLocked) flags|=0x10;
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0) flags|=0x20;
#elif CONFIG_IDF_TARGET_ESP32C3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_GPIO) flags|=0x20;
#endif
  value[2]=flags;value[3]=static_cast<uint8_t>(bootResetReason);
  put32le(value+4,capturedFrames.load());
  put32le(value+8,captureDrops.load());
  put32le(value+12,notifyRejected.load());
  put32le(value+16,controlDrops.load());
  put32le(value+20,ESP.getFreeHeap());
  put32le(value+24,ESP.getMinFreeHeap());
  put32le(value+28,millis()/1000u);
  const uint16_t reason=lastDisconnectReason.load(), status=lastNotifyStatus.load();
  value[32]=reason&255;value[33]=reason>>8;
  value[34]=status&255;value[35]=status>>8;
  put32le(value+36,linkDisconnects.load());
  put32le(value+40,lastDisconnectAt.load());
  put32le(value+44,lastNotifyError.load());
  diagnosticsCharacteristic->setValue(value,sizeof(value));
}
// Optional recovery protocol. Buffers are volatile and owned by one app session.
namespace SynapRecovery {
using StoredFrame = AudioFrame; // Recovery always retains uncompressed PCM.
class Ring {
 public:
  StoredFrame* frames=nullptr;
  uint16_t capacity=0,head=0,count=0,cursor=0;
  void reset() { head=count=cursor=0; }
  bool push(const StoredFrame& frame) {
    if (!capacity) return false;
    bool lost=false;
    if (count==capacity) { head=(head+1)%capacity; if(cursor) --cursor; else lost=true; --count; }
    frames[(head+count)%capacity]=frame; ++count; return lost;
  }
  bool peek(StoredFrame& frame) const { if(cursor>=count) return false; frame=frames[(head+cursor)%capacity]; return true; }
  void sent(uint16_t sequence) { if(cursor<count && frames[(head+cursor)%capacity].sequence==sequence) ++cursor; }
  void after(uint16_t sequence) {
    cursor=0;
    for(uint16_t i=0;i<count;++i) if(frames[(head+i)%capacity].sequence==sequence) { cursor=i+1; return; }
  }
};
}
SynapRecovery::Ring recoveryRing;
SemaphoreHandle_t recoveryMutex=nullptr;
std::atomic<bool> recoveryEnabled{false}, recoveryWaiting{false}, recoveryFinishing{false};
uint32_t recoveryFinishAt=0;
std::atomic<uint32_t> recoveryWaitingAt{0};
BLECharacteristic* recoveryCharacteristic=nullptr;
uint8_t recoveryToken[8]={};
uint8_t recoveryReplayAck=0; // Protected by recoveryMutex; acknowledges connected replay.
struct RecoveryRequest { uint8_t command=0,token[8]={}; uint16_t sequence=0; uint32_t connection=0; };
RecoveryRequest recoveryRequest;
class RecoveryGuard {
 public:
  RecoveryGuard() { xSemaphoreTake(recoveryMutex,portMAX_DELAY); }
  ~RecoveryGuard() { xSemaphoreGive(recoveryMutex); }
};
void encodeImaAdpcm(const int16_t* samples, uint8_t* output);
bool sendCapturedFrame(const AudioFrame& frame, uint32_t paceUs);
bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t* encoded,uint32_t paceUs,bool pcm);

void initializeRecovery() {
  recoveryMutex=xSemaphoreCreateMutex();
  if(!recoveryMutex)return;
#if CONFIG_IDF_TARGET_ESP32S3
  recoveryRing.frames=static_cast<SynapRecovery::StoredFrame*>(heap_caps_malloc(600*sizeof(SynapRecovery::StoredFrame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(recoveryRing.frames)recoveryRing.capacity=600;
#endif
  // 25 PCM frames use about 40 KB, preserving the previous internal-RAM budget.
  // S3 PSRAM can retain 30 seconds; C3/no-PSRAM retains 1.25 seconds.
  if(!recoveryRing.frames && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>140000) {
    recoveryRing.frames=static_cast<SynapRecovery::StoredFrame*>(heap_caps_malloc(25*sizeof(SynapRecovery::StoredFrame),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(recoveryRing.frames)recoveryRing.capacity=25;
  }
}
void resetRecovery(bool disarm=false);
void resetRecovery(bool disarm) {
  recoveryWaiting=false; recoveryWaitingAt=0; recoveryFinishing=false; recoveryFinishAt=0;
  if(disarm)recoveryEnabled=false;
  if(recoveryMutex) { RecoveryGuard guard; recoveryRing.reset(); recoveryReplayAck=0; if(disarm)memset(recoveryToken,0,sizeof(recoveryToken)); }
}
void retainRecoveryFrame(const AudioFrame& frame) {
  if(!recoveryEnabled.load())return;
  RecoveryGuard guard;
  if(streamingEnabled.load() && frame.generation==streamGeneration.load() && recoveryRing.push(frame)) ++captureDrops;
}
bool recoveryCanSend() {
  if(!recoveryEnabled.load() || recoveryWaiting.load() || !deviceConnected.load())return false;
  RecoveryGuard guard; return recoveryRing.cursor<recoveryRing.count;
}
bool sendRecoveryFrame() {
  const uint32_t connection=connectionGeneration.load();
  SynapRecovery::StoredFrame frame; uint16_t pending=0;
  { RecoveryGuard guard; if(!recoveryRing.peek(frame))return false; pending=recoveryRing.count-recoveryRing.cursor; }
  if(recoveryWaiting.load() || !streamingEnabled.load() || !deviceConnected.load())return false;
  const uint32_t pace=pending>4 && chunksPerFrame.load()<=5 ? 30000u : 45000u;
  const uint32_t rejectedBefore=notifyRejected.load();
  const bool sent=sendCapturedFrame(frame,pace);
  if(sent && !recoveryWaiting.load() && connection==connectionGeneration.load()) { RecoveryGuard guard; recoveryRing.sent(frame.sequence); }
  else if(!sent && !recoveryWaiting.load() && deviceConnected.load() && streamingEnabled.load() && frame.generation==streamGeneration.load() && connection==connectionGeneration.load()) {
    // A full controller queue is temporary. Retain this frame for a later send;
    // advancing the cursor here would discard audio the BLE stack never accepted.
    if(notifyRejected.load()!=rejectedBefore)vTaskDelay(pdMS_TO_TICKS(30));
    else requestStreamError(ErrorCode::TRANSPORT_CHANGED,frame.generation);
  }
  return sent;
}
void processRecoveryRequest() {
  if(!recoveryMutex)return;
  RecoveryRequest request;
  { RecoveryGuard guard; request=recoveryRequest; recoveryRequest.command=0; }
  if(!request.command || request.connection!=connectionGeneration.load() || !deviceConnected.load() || otaBusy() || sleepPending)return;
  if(request.command==1 && !streamingEnabled.load() && recoveryRing.capacity) {
    RecoveryGuard guard; memcpy(recoveryToken,request.token,8); recoveryEnabled=true; recoveryRing.reset(); recoveryReplayAck=0;
  } else if(request.command==2 && recoveryEnabled.load() && recoveryWaiting.load() && streamingEnabled.load()) {
    { RecoveryGuard guard; if(memcmp(request.token,recoveryToken,8)!=0)return; }
#if defined(CONFIG_BLUEDROID_ENABLED)
    if(!audioCccd || !audioCccd->getNotifications())return;
#endif
    if(!configureTransportFromPeerMtu())return;
    { RecoveryGuard guard; recoveryRing.after(request.sequence); }
    recoveryWaitingAt=0; recoveryWaiting=false;
    setDeviceState(DeviceState::STREAMING,ErrorCode::NONE); updateStatusCharacteristic(true);
  } else if(request.command==3 && recoveryEnabled.load() && !recoveryWaiting.load() &&
            streamingEnabled.load() && !recoveryFinishing.load()) {
    // A suspended web view can lose notifications while the native BLE link
    // remains connected. Rewind retained samples without restarting capture or
    // renegotiating its format. A send already in flight can only advance its
    // own sequence (Ring::sent), never skip the rewound recovery cursor.
#if defined(CONFIG_BLUEDROID_ENABLED)
    if(!audioCccd || !audioCccd->getNotifications())return;
#endif
    RecoveryGuard guard;
    if(memcmp(request.token,recoveryToken,8)!=0)return;
    recoveryRing.after(request.sequence);
    ++recoveryReplayAck;
  }
}
void updateRecoveryStatus(BLECharacteristic* characteristic,bool notify) {
    uint8_t value[16]={0x52,1,0,0};
    if(recoveryMutex) {
      RecoveryGuard guard;
      value[2]=(recoveryRing.capacity?0x11:0)|(recoveryEnabled.load()?2:0)|(recoveryWaiting.load()?4:0)|(recoveryFinishing.load()?8:0);
      value[3]=recoveryReplayAck;
      value[4]=recoveryRing.capacity&255; value[5]=recoveryRing.capacity>>8;
      const uint16_t pending=recoveryRing.count-recoveryRing.cursor;
      value[6]=pending&255; value[7]=pending>>8;
      const uint32_t generation=streamGeneration.load();
      for(uint8_t i=0;i<4;++i)value[8+i]=uint8_t(generation>>(8*i));
      uint32_t tokenHash=2166136261u;
      for(uint8_t byte:recoveryToken)tokenHash=(tokenHash^byte)*16777619u;
      for(uint8_t i=0;i<4;++i)value[12+i]=uint8_t(tokenHash>>(8*i));
    }
    characteristic->setValue(value,sizeof(value));
    if(notify && deviceConnected.load())characteristic->notify();
}
void finishBufferedRecording() {
  if(recoveryFinishing.load())return;
  recoveryFinishing=true;recoveryFinishAt=millis();
#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  updateRecoveryStatus(recoveryCharacteristic,true);
}
class RecoveryCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override { updateRecoveryStatus(characteristic,false); }
  void onWrite(BLECharacteristic* characteristic) override {
    if(!recoveryMutex)return;
    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();
    if(!data || !((size==9 && data[0]==1)||(size==11 && (data[0]==2 || data[0]==3))))return;
    RecoveryGuard guard;
    recoveryRequest.command=data[0];memcpy(recoveryRequest.token,data+1,8);
    recoveryRequest.sequence=size==11 ? uint16_t(data[9])|(uint16_t(data[10])<<8) : 0;
    recoveryRequest.connection=connectionGeneration.load();
  }
};

void stopStreaming(ErrorCode reason) {
  streamingEnabled.store(false);
  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.
  if (audioFrameQueue) xQueueReset(audioFrameQueue);
#if USE_REAL_I2S_MIC
#if SYNAP_CHAKSHU
  if (!mediaBusy()) stopMicrophone();
#else
  stopMicrophone();
#endif
#endif
  // Acknowledge STOP only after the final in-flight notification has returned.
  while (transmitterActive.load()) vTaskDelay(1);
  resetRecovery(!deviceConnected.load());
  applyCpuPowerProfile(false);
  if (!deviceConnected.load()) setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  else if (reason == ErrorCode::NONE) setDeviceState(DeviceState::CONNECTED_IDLE, reason);
  else setDeviceState(DeviceState::ERROR, reason);
  updateStatusCharacteristic(true);
}
bool configureTransportFromPeerMtu() {
  if (!deviceConnected.load() || !bleServer) return false;
  peerMtu = bleServer->getPeerMTU(bleServer->getConnId());
  if (peerMtu < 23) peerMtu = 23;
  attValueCapacity = peerMtu - 3;
  audioPayloadBytes = 0; chunksPerFrame = 0; pcmTransport=false;
  if (peerMtu < MIN_REQUIRED_MTU) return false;
  const uint16_t available = attValueCapacity - AUDIO_HEADER_BYTES;
  uint16_t bounded = available < MAX_AUDIO_PAYLOAD_BYTES ? available : MAX_AUDIO_PAYLOAD_BYTES;
  // Packet-size eligibility is not a throughput guarantee; drop/reject counters
  // remain visible. Keep the selected format stable until START or RESUME.
  pcmTransport=peerMtu>=PCM_MIN_MTU;
  if(pcmTransport.load())bounded&=~1u;
  const uint16_t frameBytes=pcmTransport.load()?AUDIO_BYTES_PER_FRAME:ADPCM_BYTES_PER_FRAME;
  chunksPerFrame = (frameBytes + bounded - 1) / bounded;
  if (chunksPerFrame > MAX_CHUNKS_PER_FRAME) return false;
  uint16_t payload=(frameBytes + chunksPerFrame - 1) / chunksPerFrame;
  if(pcmTransport.load())payload=(payload+1u)&~1u; // Never split a PCM16 sample.
  audioPayloadBytes=payload;
  return audioPayloadBytes + AUDIO_HEADER_BYTES <= attValueCapacity;
}
void startStreaming(uint8_t version) {
#if SYNAP_CHAKSHU
  if (mediaBusy()) { updateStatusCharacteristic(true);return; }
#endif
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (!deviceConnected.load()) return;
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  // Repeated START is idempotent; it must not reset an active take's sequence.
  if (streamingEnabled.load()) { updateStatusCharacteristic(true); return; }
#if defined(CONFIG_BLUEDROID_ENABLED)
  if (!audioCccd || !audioCccd->getNotifications()) {
    stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED); return;
  }
#endif
  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }
  applyCpuPowerProfile(true);
#if USE_REAL_I2S_MIC
  if (!startMicrophone()) { stopStreaming(ErrorCode::AUDIO_SOURCE_FAILED); return; }
#endif
  xQueueReset(audioFrameQueue);
  ++streamGeneration;
  capturedFrames=0; captureDrops=0; notifyRejected=0;
  resetRecovery();
  setDeviceState(DeviceState::STREAMING, ErrorCode::NONE);
  streamingEnabled.store(true);
  if (captureTaskHandle) xTaskNotifyGive(captureTaskHandle);
  updateStatusCharacteristic(true);
}
