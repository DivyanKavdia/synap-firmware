// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
static std::atomic<uint8_t> odysseyPersistedRecordStage{0};
static std::atomic<uint32_t> odysseyPersistedRecordBytes{0};
static std::atomic<bool> odysseyPersistedRecordLoaded{false};

static void odysseyLoadPersistedRecordFailure() {
  if (odysseyPersistedRecordLoaded.exchange(true)) return;
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",true)) return;
  uint32_t record[3]{};
  if (prefs.getBytesLength("last")==sizeof(record) &&
      prefs.getBytes("last",record,sizeof(record))==sizeof(record) && record[0]==1u) {
    odysseyPersistedRecordStage=uint8_t(record[1]&255u);
    odysseyPersistedRecordBytes=record[2];
  }
  prefs.end();
}
uint8_t odysseyLastRecordFailureStage() {
  odysseyLoadPersistedRecordFailure();
  return odysseyPersistedRecordStage.load();
}
uint32_t odysseyLastRecordFailureBytes() {
  odysseyLoadPersistedRecordFailure();
  return odysseyPersistedRecordBytes.load();
}
static void odysseyPersistRecordFailure(uint8_t stage,uint32_t bytes) {
  odysseyPersistedRecordLoaded=true;
  odysseyPersistedRecordStage=stage;
  odysseyPersistedRecordBytes=bytes;
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",false)) return;
  const uint32_t record[3]={stage?1u:0u,uint32_t(stage),bytes};
  (void)prefs.putBytes("last",record,sizeof(record));
  prefs.end();
}

static void odysseyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static bool odysseyFinalizeWav(FILE* file,uint8_t* header,uint32_t bytes) {
  // Capture stays strictly sequential. Flush audio once, then rewrite the
  // 44-byte RIFF header in place immediately before close.
  if (fflush(file)!=0) return false;
  odysseyWavHeader(header,bytes);
  if (fseek(file,0,SEEK_SET)!=0) return false;
  if (fwrite(header,1,44,file)!=44) return false;
  return fflush(file)==0;
}
static uint8_t odysseyCreateFailureStage(int error) {
  // Preserve the actual FAT/VFS create error in the existing one-byte
  // diagnostic field. These values are emitted only when fopen() fails.
  switch (error) {
    case EIO: return 49;
    case ENODEV: return 50;
    case EMFILE:
    case ENFILE: return 51;
    case ENOSPC: return 52;
    case EROFS: return 53;
    default: return 54;
  }
}
static uint8_t odysseyWriteFailureStage(int error) {
  switch (error) {
    case EIO: return 55;
    case ENODEV: return 56;
    case ENOSPC: return 57;
    case EROFS: return 58;
    default: return 59;
  }
}
static void odysseyRecordTake() {
  bool failed=false;
  uint8_t failureStage=0;
  uint32_t bytes=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  alignas(4) uint8_t sector[512]{};
  size_t sectorUsed=0;
  uint32_t sectorPcmBytes=0;
  FILE* file=nullptr;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) { failed=true;failureStage=40; }

  if (!failed) {
    // A 64-bit random suffix makes a collision negligible, so do not precede
    // creation with stat(). A failed metadata read must never block a write
    // that the card may still be able to perform.
    snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
      static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
      failed=true;failureStage=41;
    } else {
      errno=0;
      // Match the boot write probe's proven access mode. Read permission is
      // unnecessary: fseek()+fwrite() can rewrite the header on a write stream.
      file=fopen(fullPath,"wb");
      if (!file) {
        const int openError=errno;
        failed=true;failureStage=odysseyCreateFailureStage(openError);
        Serial.printf("[SD] WAV create failed errno=%d stage=%u path=%s\n",
          openError,unsigned(failureStage),fullPath);
      } else if (setvbuf(file,nullptr,_IONBF,0)!=0) {
        failed=true;failureStage=42;
      }
    }
  }

  odysseyWavHeader(header,0);
  if (!failed) {
    memcpy(sector,header,sizeof(header));
    sectorUsed=sizeof(header);
    sectorPcmBytes=0;
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    if (!startMicrophone()) { failed=true;failureStage=43; }
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true;failureStage=43; break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (bytes>0xffffff00u-sizeof(pcm)) break; // RIFF length is 32-bit.
      const uint8_t* input=reinterpret_cast<const uint8_t*>(pcm);
      size_t remaining=sizeof(pcm);
      while (!failed && remaining) {
        const size_t room=sizeof(sector)-sectorUsed;
        const size_t take=remaining<room?remaining:room;
        memcpy(sector+sectorUsed,input,take);
        sectorUsed+=take;
        sectorPcmBytes+=uint32_t(take);
        input+=take;
        remaining-=take;
        if (sectorUsed==sizeof(sector)) {
          errno=0;
          const size_t written=fwrite(sector,1,sizeof(sector),file);
          if (written!=sizeof(sector)) {
            const int writeError=errno;
            failed=true;failureStage=odysseyWriteFailureStage(writeError);
            Serial.printf("[SD] PCM sector write failed errno=%d stage=%u wrote=%u pcm=%lu\n",
              writeError,unsigned(failureStage),unsigned(written),
              static_cast<unsigned long>(bytes));
            break;
          }
          bytes+=sectorPcmBytes;
          sectorUsed=0;
          sectorPcmBytes=0;
        }
      }

      // Capture performs only sequential 512-byte writes. No fseek, fflush or
      // header rewrite occurs until the take is stopped.
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=43;
#endif

  if (file) {
    if (!failed && sectorUsed) {
      errno=0;
      const size_t written=fwrite(sector,1,sectorUsed,file);
      if (written!=sectorUsed) {
        const int writeError=errno;
        failed=true;failureStage=odysseyWriteFailureStage(writeError);
        Serial.printf("[SD] final PCM sector failed errno=%d stage=%u wrote=%u expected=%u\n",
          writeError,unsigned(failureStage),unsigned(written),unsigned(sectorUsed));
      } else {
        bytes+=sectorPcmBytes;
        sectorUsed=0;
        sectorPcmBytes=0;
      }
    }
    if (!failed && !odysseyFinalizeWav(file,header,bytes)) {
      failed=true;
      if (!failureStage) failureStage=46;
    }
    if (fclose(file)!=0) {
      failed=true;
      if (!failureStage) failureStage=47;
    }
    file=nullptr;
  }

  if (!failed && bytes==0) failureStage=48;
  Serial.printf("[SD] local audio %s: %s, %lu PCM bytes stage=%u%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),
    unsigned(failureStage),failed?" (mount retained for explicit recovery)":"");
  // Diagnostics are published only after the historical 1631 I/O sequence has
  // completed, so they cannot change the write timing being measured.
  if (failed || bytes==0) {
    const uint8_t persistedStage=failureStage?failureStage:40;
    odysseyPersistRecordFailure(persistedStage,bytes);
    odysseySdBootState=2;
    odysseySdProbeStage=persistedStage;
    odysseyRecordFaultAt=millis();
  } else {
    odysseyPersistRecordFailure(0,0);
    odysseySdBootState=1;
    odysseySdProbeStage=6;
  }
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
