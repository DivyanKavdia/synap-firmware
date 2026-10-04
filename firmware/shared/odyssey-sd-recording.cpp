// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
static void odysseyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static bool odysseyCheckpointWav(FILE* file,uint8_t* header,uint32_t bytes) {
  odysseyWavHeader(header,bytes);
  if (fseek(file,0,SEEK_SET)!=0) return false;
  if (fwrite(header,1,44,file)!=44) return false;
  if (fseek(file,long(44u+bytes),SEEK_SET)!=0) return false;
  return fflush(file)==0;
}
static bool odysseyRecordTake() {
  bool failed=false,storageFault=false;
  uint32_t bytes=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  FILE* file=nullptr;
  bool created=false;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) failed=true;

  if (!failed) {
    struct stat existing{};
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) { failed=true; break; }
      if (stat(fullPath,&existing)!=0) {
        if (errno!=ENOENT) {
          failed=true;storageFault=true;odysseySdMarkVfsFailure();break;
        }
        file=fopen(fullPath,"wb+");
        if (file) { created=true; break; }
        failed=true;storageFault=true;odysseySdMarkVfsFailure();break;
      }
    }
    if (!file) failed=true;
  }

  odysseyWavHeader(header,0);
  if (!failed) {
    // Persist a valid zero-audio WAV boundary immediately. A directory entry
    // must never remain 0 B merely because stdio had not flushed yet.
    if (fwrite(header,1,sizeof(header),file)!=sizeof(header) || fflush(file)!=0) {
      failed=true;storageFault=true;odysseySdMarkVfsFailure();
    }
  }

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
      bool microphoneRecoveryUsed=false;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) {
            // Match the connected recorder's bounded recovery. Some C3 I2S
            // starts succeed before samples are immediately available; one
            // driver restart prevents that transient from becoming a failed
            // offline take and a red fault indication.
            if (!microphoneRecoveryUsed && !odysseyStopRequested.load()) {
              microphoneRecoveryUsed=true;
              Serial.println("[MIC] offline SD empty I2S reads; restarting capture driver");
              stopMicrophone();
              delay(35);
              if (odysseyStopRequested.load()) break;
              if (startMicrophone()) { received=0; emptyReads=0; continue; }
            }
            if (!odysseyStopRequested.load()) failed=true;
            break;
          }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (bytes>0xffffff00u-sizeof(pcm)) break; // RIFF length is 32-bit.
      const size_t written=fwrite(pcm,1,sizeof(pcm),file);
      bytes+=uint32_t(written & ~size_t(1));
      if (written!=sizeof(pcm)) {
        failed=true;storageFault=true;odysseySdMarkVfsFailure();break;
      }

      // Keep a recoverable WAV header on media even if power is lost mid-take.
      if (uint32_t(millis()-checkpointAt)>=2000u) {
        if (!odysseyCheckpointWav(file,header,bytes)) {
          failed=true;storageFault=true;odysseySdMarkVfsFailure();break;
        }
        checkpointAt=millis();
      }
    }
    stopMicrophone();
  }
#else
  failed=true;
#endif

  if (file) {
    if (!odysseyCheckpointWav(file,header,bytes)) {
      failed=true;storageFault=true;odysseySdMarkVfsFailure();
    }
    if (fclose(file)!=0) {
      failed=true;storageFault=true;odysseySdMarkVfsFailure();
    }
    file=nullptr;
  }

  long finalSize=-1;
  if (created) {
    struct stat finalStat{};
    if (stat(fullPath,&finalStat)==0 && S_ISREG(finalStat.st_mode)) finalSize=long(finalStat.st_size);
    const uint64_t expectedSize=uint64_t(sizeof(header))+uint64_t(bytes);
    if (finalSize<0 || uint64_t(finalSize)!=expectedSize) {
      failed=true;storageFault=true;odysseySdMarkVfsFailure();
    }
  }

  Serial.printf("[SD] local audio %s: %s, %lu PCM bytes, file bytes=%ld%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),finalSize,
    failed?" (storage recovery may follow)":"");
  if (failed || bytes==0 || finalSize<=long(sizeof(header))) odysseyRecordFaultAt=millis();
  return storageFault;
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  const bool storageFault=odysseyRecordTake();
  // odysseyRecordTake() returned, so its SD guard has released the mutex.
  // Only now is it safe to tear down/quiesce a faulted mounted session.
  odysseyRecording=false;
  odysseyStopRequested=false;
  if (storageFault) {
    (void)odysseySdQuiesceFaultedSession(750u);
    // One bounded background recovery is appropriate here: this fault came
    // from an explicit offline recording action, not passive catalogue polling.
    odysseySdRequestRecovery();
    Serial.println("[SD] offline recording storage fault; cleanup complete, recovery requested");
  }
  applyCpuPowerProfile(false);
  updateStatusLed(true);
  vTaskDelete(nullptr);
}
bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs) {
  if (!odysseyRecording.load()) return true;
  // Connected PWA capture owns future I2S access, but the disconnected SD take
  // must close its WAV header/file before the microphone can change owners.
  odysseyStopRequested=true;
  Serial.println("[SD] BLE capture requested; finalizing local audio before live stream");
  const uint32_t started=millis();
  while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
  if (odysseyRecording.load()) {
    Serial.println("[SD] local audio did not finalize before BLE capture deadline");
    return false;
  }
  Serial.println("[SD] local audio finalized; microphone released to BLE capture");
  return true;
}
void odysseyToggleRecording() {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;
  if (!odysseySdReady()) {
    odysseySdRequestRecovery();
    odysseyRecordFaultAt=millis();
    updateStatusLed(true);
    Serial.println("[TOUCH] SD unavailable; requesting background recovery. Retry double tap after mount.");
    return;
  }
  odysseyStopRequested=false;
  odysseyRecordingStartedAt=millis();
  odysseyRecordFaultAt=0;
  odysseyRecording=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseyStopRequested=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> SD audio START");
}
#endif
