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
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=12800u; // 8 PCM frames / 25 SD sectors
static constexpr uint32_t ODYSSEY_WAV_CHECKPOINT_MS=15000u;

static bool odysseyCheckpointWav(FILE* file,uint8_t* header,uint32_t bytes) {
  odysseyWavHeader(header,bytes);
  if (fseek(file,0,SEEK_SET)!=0) return false;
  if (fwrite(header,1,44,file)!=44) return false;
  if (fseek(file,long(44u+bytes),SEEK_SET)!=0) return false;
  return fflush(file)==0;
}

static bool odysseyFlushPcmBuffer(FILE* file,uint8_t* buffer,size_t& buffered,uint32_t& bytes) {
  if (!buffered) return true;
  const size_t pending=buffered;
  const size_t written=fwrite(buffer,1,pending,file);
  bytes+=uint32_t(written & ~size_t(1));
  buffered=0;
  return written==pending;
}

static void odysseyRecordTake() {
  bool failed=false,storageFailed=false;
  uint32_t bytes=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  FILE* file=nullptr;
  uint8_t* writeBuffer=nullptr;
  size_t bufferedBytes=0;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) { failed=true;storageFailed=true; }

  if (!failed) {
    struct stat existing{};
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) { failed=true;storageFailed=true; break; }
      if (stat(fullPath,&existing)!=0 && errno==ENOENT) {
        file=fopen(fullPath,"wb+");
        if (file) break;
      }
    }
    if (!file) { failed=true;storageFailed=true; }
  }

  // Keep the buffer off the FreeRTOS task stack. 12.8 KB is an exact multiple
  // of both the 1600-byte PCM frame and the 512-byte SD sector, so routine card
  // writes are large and sector-aligned instead of one write per audio frame.
  if (!failed) {
    writeBuffer=static_cast<uint8_t*>(malloc(ODYSSEY_SD_WRITE_BUFFER_BYTES));
    if (!writeBuffer) failed=true;
  }

  odysseyWavHeader(header,0);
  if (!failed && fwrite(header,1,sizeof(header),file)!=sizeof(header)) { failed=true;storageFailed=true; }

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
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true; break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (uint64_t(bytes)+uint64_t(bufferedBytes)+sizeof(pcm)>0xffffff00ull) break;

      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        if (!odysseyFlushPcmBuffer(file,writeBuffer,bufferedBytes,bytes)) {
          failed=true;storageFailed=true;break;
        }
      }
      memcpy(writeBuffer+bufferedBytes,pcm,sizeof(pcm));
      bufferedBytes+=sizeof(pcm);

      if (bufferedBytes==ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        if (!odysseyFlushPcmBuffer(file,writeBuffer,bufferedBytes,bytes)) {
          failed=true;storageFailed=true;break;
        }
      }

      // A 15-second checkpoint retains crash recoverability while avoiding the
      // old 2-second seek/header/flush cycle that repeatedly forced FAT writes.
      if (uint32_t(millis()-checkpointAt)>=ODYSSEY_WAV_CHECKPOINT_MS) {
        if (!odysseyFlushPcmBuffer(file,writeBuffer,bufferedBytes,bytes) ||
            !odysseyCheckpointWav(file,header,bytes)) {
          failed=true;storageFailed=true;break;
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
    if (!odysseyFlushPcmBuffer(file,writeBuffer,bufferedBytes,bytes)) {
      failed=true;storageFailed=true;
    }
    if (!odysseyCheckpointWav(file,header,bytes)) { failed=true;storageFailed=true; }
    if (fclose(file)!=0) { failed=true;storageFailed=true; }
    file=nullptr;
  }
  if (writeBuffer) {
    free(writeBuffer);
    writeBuffer=nullptr;
  }

  Serial.printf("[SD] local audio %s: %s, %lu PCM bytes, buffered=%u checkpoint=%lus%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),
    unsigned(ODYSSEY_SD_WRITE_BUFFER_BYTES),
    static_cast<unsigned long>(ODYSSEY_WAV_CHECKPOINT_MS/1000u),
    failed?" (mount retained for explicit recovery)":"");
  // Surface media I/O failure immediately instead of advertising SD-ready until
  // the next catalogue happens to discover the broken VFS.
  if (storageFailed) odysseySdMarkVfsFailure();
  // A mounted SD card can still fail to allocate RAM, open/start the microphone,
  // or return zero audio without proving that the filesystem itself failed.
  if (failed || bytes==0) odysseyRecordFaultAt=millis();
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  odysseyRecordTake();
  odysseyRecording=false;
  odysseyStopRequested=false;
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
