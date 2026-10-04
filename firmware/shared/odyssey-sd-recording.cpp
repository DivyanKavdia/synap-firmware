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
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=8192u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_SECTOR_BYTES=512u;
static constexpr size_t ODYSSEY_WAV_HEADER_BYTES=44u;
static constexpr uint32_t ODYSSEY_WAV_CHECKPOINT_MS=15000u;
static uint8_t odysseySdWriteBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];
static std::atomic<uint8_t> odysseyRecordFailureStage{0};
static std::atomic<uint32_t> odysseyRecordLastBytes{0};

uint8_t odysseySdRecordFailureStage() { return odysseyRecordFailureStage.load(); }
uint32_t odysseySdRecordLastBytes() { return odysseyRecordLastBytes.load(); }

static bool odysseyCheckpointWav(FILE* file,uint8_t* header,uint32_t bytes) {
  odysseyWavHeader(header,bytes);
  if (fseek(file,0,SEEK_SET)!=0) return false;
  if (fwrite(header,1,ODYSSEY_WAV_HEADER_BYTES,file)!=ODYSSEY_WAV_HEADER_BYTES) return false;
  if (fseek(file,long(ODYSSEY_WAV_HEADER_BYTES+bytes),SEEK_SET)!=0) return false;
  return fflush(file)==0;
}

static bool odysseyWriteBufferedChunk(FILE* file,uint8_t* buffer,size_t& buffered,
    size_t count,uint32_t& bytes) {
  if (!count || count>buffered) return count==0;
  const size_t written=fwrite(buffer,1,count,file);
  if (written) {
    bytes+=uint32_t(written);
    buffered-=written;
    if (buffered) memmove(buffer,buffer+written,buffered);
  }
  return written==count;
}

static bool odysseyDrainPcmBuffer(FILE* file,uint8_t* buffer,size_t& buffered,
    uint32_t& bytes,bool finalFlush) {
  while (buffered) {
    const size_t fileOffset=ODYSSEY_WAV_HEADER_BYTES+size_t(bytes);
    const size_t sectorOffset=fileOffset&(ODYSSEY_SD_SECTOR_BYTES-1u);
    size_t chunk=0;

    // The WAV payload starts at byte 44. Complete that first physical sector,
    // then keep normal data writes sector-aligned without changing WAV format.
    if (sectorOffset) {
      const size_t toBoundary=ODYSSEY_SD_SECTOR_BYTES-sectorOffset;
      if (!finalFlush && buffered<toBoundary) return true;
      chunk=std::min(buffered,toBoundary);
    } else {
      const size_t aligned=buffered&~(ODYSSEY_SD_SECTOR_BYTES-1u);
      if (aligned) chunk=std::min(aligned,ODYSSEY_SD_WRITE_CHUNK_BYTES);
      else if (finalFlush) chunk=buffered;
      else return true;
    }

    if (!odysseyWriteBufferedChunk(file,buffer,buffered,chunk,bytes)) return false;
  }
  return true;
}

static void odysseyRecordTake() {
  bool failed=false,storageFailed=false;
  uint8_t failureStage=0;
  uint32_t bytes=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  FILE* file=nullptr;
  size_t bufferedBytes=0;

  odysseyRecordFailureStage=0;
  odysseyRecordLastBytes=0;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) {
    failed=true;storageFailed=true;failureStage=1;
  }

  if (!failed) {
    struct stat existing{};
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
        failed=true;storageFailed=true;failureStage=2;break;
      }
      if (stat(fullPath,&existing)!=0 && errno==ENOENT) {
        file=fopen(fullPath,"wb+");
        if (file) break;
      }
    }
    if (!file) { failed=true;storageFailed=true;failureStage=2; }
  }

  // We own buffering explicitly. Disable stdio's hidden buffer so the 4 KiB
  // sector-aware writes below are the writes presented to the VFS/FatFs layer.
  if (!failed && setvbuf(file,nullptr,_IONBF,0)!=0) {
    failed=true;failureStage=3;
  }

  odysseyWavHeader(header,0);
  if (!failed && fwrite(header,1,sizeof(header),file)!=sizeof(header)) {
    failed=true;storageFailed=true;failureStage=3;
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    if (!startMicrophone()) { failed=true;failureStage=4; }
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    uint32_t checkpointAt=millis();
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true;failureStage=4;break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (uint64_t(bytes)+uint64_t(bufferedBytes)+sizeof(pcm)>0xffffff00ull) break;

      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=5;break;
        }
      }
      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        failed=true;failureStage=5;break;
      }
      memcpy(odysseySdWriteBuffer+bufferedBytes,pcm,sizeof(pcm));
      bufferedBytes+=sizeof(pcm);

      if (bufferedBytes>=ODYSSEY_SD_WRITE_CHUNK_BYTES+ODYSSEY_SD_SECTOR_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=5;break;
        }
      }

      // Retain crash recoverability without the old 2-second metadata churn.
      if (uint32_t(millis()-checkpointAt)>=ODYSSEY_WAV_CHECKPOINT_MS) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false) ||
            !odysseyCheckpointWav(file,header,bytes)) {
          failed=true;storageFailed=true;failureStage=6;break;
        }
        checkpointAt=millis();
      }
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=4;
#endif

  if (file) {
    if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,true)) {
      failed=true;storageFailed=true;if (!failureStage) failureStage=5;
    }
    if (!odysseyCheckpointWav(file,header,bytes)) {
      failed=true;storageFailed=true;if (!failureStage) failureStage=6;
    }
    if (fclose(file)!=0) {
      failed=true;storageFailed=true;if (!failureStage) failureStage=7;
    }
    file=nullptr;
  }

  if (!failed && bytes==0) failureStage=8;
  odysseyRecordFailureStage=failureStage;
  odysseyRecordLastBytes=bytes;

  Serial.printf("[SD] local audio %s: %s, %lu PCM bytes, stage=%u buffer=%u chunk=%u checkpoint=%lus%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),unsigned(failureStage),
    unsigned(ODYSSEY_SD_WRITE_BUFFER_BYTES),unsigned(ODYSSEY_SD_WRITE_CHUNK_BYTES),
    static_cast<unsigned long>(ODYSSEY_WAV_CHECKPOINT_MS/1000u),
    failed?" (mount retained for explicit recovery)":"");
  if (storageFailed) odysseySdMarkVfsFailure();
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
