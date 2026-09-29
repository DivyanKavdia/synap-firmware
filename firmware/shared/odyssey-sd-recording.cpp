// C3 local audio owns its file and microphone until finalization. BLE connection
// changes never redirect a take; no local PCM enters the app recovery/notify queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
static void odysseyWavHeader(uint8_t* h, uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4); put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8); put32le(h+16,16);
  h[20]=1; h[22]=1; put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2); h[32]=2; h[34]=16;
  memcpy(h+36,"data",4); put32le(h+40,bytes);
}
static void odysseyRecordTask(void*) {
  bool failed=false;
  File file;
  uint32_t bytes=0;
  char path[64]={};
  // Retry on every offline start, including cards fitted after boot or a prior
  // write fault. This never formats media or deletes an existing recording.
  odysseyDetectSdCard();
  if (odysseySdDetectionState()!=1) failed=true;
  if (!failed && !SD.exists("/synap") && !SD.mkdir("/synap")) failed=true;
  if (!failed) {
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(path,sizeof(path),"/synap/odyssey_audio_%08lx_%08lx.wav",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
      if (!SD.exists(path)) { file=SD.open(path,FILE_WRITE); break; }
    }
    if (!file) failed=true;
  }
  uint8_t header[44];
  odysseyWavHeader(header,0);
  if (!failed && file.write(header,sizeof(header))!=sizeof(header)) failed=true;
#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    if (!startMicrophone()) failed=true;
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    uint32_t checkpointAt=millis();
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) { if (++emptyReads>=3) { failed=true; break; } }
        else { received+=count; emptyReads=0; }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      // Bound RIFF's 32-bit length; a full card ends this take safely.
      if (bytes>0xffffff00u-sizeof(pcm)) break;
      const size_t written=file.write(reinterpret_cast<const uint8_t*>(pcm),sizeof(pcm));
      bytes+=written & ~size_t(1);
      if (written!=sizeof(pcm)) { failed=true; break; }
      // Keep the on-disk header recoverable up to the last checkpoint on power loss.
      if (uint32_t(millis()-checkpointAt)>=2000u) {
        odysseyWavHeader(header,bytes);
        if (!file.seek(0) || file.write(header,44)!=44 || !file.seek(44+bytes)) { failed=true; break; }
        file.flush(); checkpointAt=millis();
      }
    }
    stopMicrophone();
  }
#else
  failed=true; // Never silently save synthetic audio as a real offline take.
#endif
  if (file) {
    odysseyWavHeader(header,bytes);
    if (!file.seek(0) || file.write(header,44)!=44) failed=true;
    file.flush(); file.close();
  }
  if (failed) { odysseySdBootState=2; SD.end(); }
  Serial.printf("[SD] local audio %s: %s, %lu PCM bytes\n",failed?"failed":"saved",path,
    static_cast<unsigned long>(bytes));
  // No microphone/file work is allowed after releasing ownership.
  odysseyRecording=false;
  vTaskDelete(nullptr);
}
void odysseyToggleRecording() {
  if (odysseyRecording.load()) { odysseyStopRequested=true; return; }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;
  odysseyStopRequested=false;
  odysseyRecording=true;
  applyCpuPowerProfile(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> SD audio START");
}
#endif
