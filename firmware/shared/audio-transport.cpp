static const uint16_t IMA_STEP_TABLE[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
  34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
  157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
  724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
  2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
  10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t IMA_INDEX_TABLE[8] = {-1,-1,-1,-1,2,4,6,8};

void encodeImaAdpcm(const int16_t* samples, uint8_t* output) {
  int32_t predictor=samples[0];
  int32_t index=0;
  output[0]=uint8_t(predictor&255);output[1]=uint8_t((predictor>>8)&255);
  output[2]=uint8_t(index);output[3]=AUDIO_CODEC_IMA_ADPCM;
  // Each low nibble assignment initializes its byte, including the final padding nibble.
  for (uint16_t sampleIndex=1;sampleIndex<SAMPLES_PER_FRAME;++sampleIndex) {
    const int32_t step=IMA_STEP_TABLE[index];
    int32_t difference=int32_t(samples[sampleIndex])-predictor;
    uint8_t code=0;
    if (difference<0) { code=8;difference=-difference; }
    int32_t delta=step>>3;
    if (difference>=step) { code|=4;difference-=step;delta+=step; }
    if (difference>=(step>>1)) { code|=2;difference-=step>>1;delta+=step>>1; }
    if (difference>=(step>>2)) { code|=1;delta+=step>>2; }
    predictor+=(code&8)?-delta:delta;
    if (predictor>32767) predictor=32767;
    if (predictor<-32768) predictor=-32768;
    index+=IMA_INDEX_TABLE[code&7];
    if (index<0) index=0;
    if (index>88) index=88;
    const uint16_t packedIndex=ADPCM_HEADER_BYTES+((sampleIndex-1)>>1);
    if ((sampleIndex-1)&1) output[packedIndex]|=uint8_t((code&15)<<4);
    else output[packedIndex]=uint8_t(code&15);
  }
}

// Only the transmitter task owns this cursor. A congested fragment is retried
// in place; restarting at zero can starve the tail of every PCM frame.
class AudioSendProgress {
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
} audioSendProgress;

bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t* encoded,uint32_t paceUs,bool pcm) {
  const uint16_t frameBytes=pcm?AUDIO_BYTES_PER_FRAME:ADPCM_BYTES_PER_FRAME;
  const uint8_t chunks=chunksPerFrame;
  const uint16_t payload=audioPayloadBytes, capacity=attValueCapacity;
  if (chunks < MIN_CHUNKS_PER_FRAME || chunks > MAX_CHUNKS_PER_FRAME ||
      !payload || payload > MAX_AUDIO_PAYLOAD_BYTES ||
      uint32_t(chunks)*payload<frameBytes || uint32_t(chunks-1)*payload>=frameBytes) return false;
  static uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  const uint32_t connection=connectionGeneration.load();
  const uint32_t replay=audioReplayGeneration.load();
  const uint8_t first=audioSendProgress.begin(generation,connection,replay,sequence,chunks,payload,pcm);
  for (uint8_t index=first; index<chunks; ++index) {
    if (!streamingEnabled.load() || !deviceConnected.load() ||
        generation != streamGeneration.load() || connection != connectionGeneration.load() || replay != audioReplayGeneration.load()) return false;
    const uint16_t offset=index*payload;
    if(offset>=frameBytes)return false;
    const uint16_t remaining=frameBytes-offset;
    const uint16_t length=remaining < payload ? remaining : payload;
    if (AUDIO_HEADER_BYTES+length > capacity) return false;
    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=pcm?PCM_AUDIO_PROTOCOL_VERSION:AUDIO_PROTOCOL_VERSION;
    packet[2]=sequence & 255; packet[3]=sequence >> 8;
    packet[4]=index; packet[5]=chunks; packet[6]=length & 255; packet[7]=length >> 8;
    memcpy(packet+AUDIO_HEADER_BYTES, encoded+offset, length);
    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);
    bool accepted=false;
    for(uint8_t attempt=0;attempt<4;++attempt) {
      if(attempt)vTaskDelay(pdMS_TO_TICKS(15u*attempt));
      if(!streamingEnabled.load() || !deviceConnected.load() ||
          generation!=streamGeneration.load() || connection!=connectionGeneration.load() || replay!=audioReplayGeneration.load())return false;
      const uint32_t rejectedBefore=notifyRejected.load();
      // In the pinned Arduino BLE library, onStatus runs before notify returns.
      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.
      audioCharacteristic->notify();
      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }
    }
    if(!accepted)return false;
    audioSendProgress.accept(index);
    // Pace from the completed attempt. An overdue notification must never cause
    // the remaining fragments to burst into the controller's congested queue.
    const uint32_t slot=static_cast<uint32_t>(index+1)*paceUs/chunks-static_cast<uint32_t>(index)*paceUs/chunks;
    const uint32_t target=micros()+slot;
    if (index+1 == chunks) {
      // Keep the frame rate bounded during queue catch-up, yielding all remaining time.
      while (static_cast<int32_t>(target-micros()) > 0) vTaskDelay(1);
    } else {
      while (static_cast<int32_t>(target-micros()) > 1000) vTaskDelay(1);
      while (static_cast<int32_t>(target-micros()) > 0) delayMicroseconds(50);
    }
  }
  audioSendProgress.reset();
  return generation == streamGeneration.load() && deviceConnected.load() && connection == connectionGeneration.load() && replay == audioReplayGeneration.load();
}
bool sendCapturedFrame(const AudioFrame& frame, uint32_t paceUs) {
  if(pcmTransport.load()) {
    // ESP32-C3/S3 PCM samples are little endian, matching the protocol-v2 wire.
    return sendEncodedFrame(frame.generation,frame.sequence,
      reinterpret_cast<const uint8_t*>(frame.samples),paceUs,true);
  }
  // Only small-MTU links use lossy compression. Recovery storage stays PCM.
  static uint8_t encoded[ADPCM_BYTES_PER_FRAME];
  encodeImaAdpcm(frame.samples,encoded);
  return sendEncodedFrame(frame.generation,frame.sequence,encoded,paceUs,false);
}
bool sendAudioFrame(const AudioFrame& frame) {
  return sendCapturedFrame(frame,45000u);
}
void transmitterTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  for (;;) {
    const bool queued=xQueueReceive(audioFrameQueue, &frame, recoveryCanSend()?0:(recoveryEnabled.load() && streamingEnabled.load()?pdMS_TO_TICKS(20):portMAX_DELAY))==pdTRUE;
    if(recoveryEnabled.load()) {
      transmitterActive.store(true);
      const bool sent=streamingEnabled.load() && recoveryCanSend() && sendRecoveryFrame();
      transmitterActive.store(false);
      if(!queued && !sent)vTaskDelay(1);
      continue;
    }
    if(!queued)continue;
    // Claim activity before checking the session so STOP cannot miss a pending send.
    transmitterActive.store(true);
    if (streamingEnabled.load() && frame.generation == streamGeneration.load()) {
      const uint32_t rejectedBefore=notifyRejected.load();
      if (!sendAudioFrame(frame) &&
          deviceConnected.load() && streamingEnabled.load() && frame.generation == streamGeneration.load()) {
        // Legacy sessions lack a recovery ring: account for the lost frame but
        // keep recording through transient queue pressure.
        if(notifyRejected.load()!=rejectedBefore)++captureDrops;
        else requestStreamError(ErrorCode::TRANSPORT_CHANGED, frame.generation);
      }
    }
    transmitterActive.store(false);
  }
}

