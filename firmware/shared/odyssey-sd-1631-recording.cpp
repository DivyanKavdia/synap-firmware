// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
static std::atomic<uint8_t> odysseyPersistedRecordStage{0};
static std::atomic<uint32_t> odysseyPersistedRecordBytes{0};
static std::atomic<bool> odysseyPersistedRecordLoaded{false};
// V2 of the existing diagnostic journal retains the precise short write.
static std::atomic<uint32_t> odysseyPersistedWriteErrno{0};
static std::atomic<uint32_t> odysseyPersistedWriteReturned{0};
static std::atomic<uint32_t> odysseyPersistedWriteExpected{0};
static std::atomic<uint32_t> odysseyPersistedWriteFerror{0};
static std::atomic<uint32_t> odysseyPersistedDriverFault{0};
extern "C" uint32_t synapSdWriteFaultCode();
extern "C" void synapSdClearWriteFaultCode();

static void odysseyLoadPersistedRecordFailure() {
  if (odysseyPersistedRecordLoaded.exchange(true)) return;
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",true)) return;
  uint32_t record[7]{};
  const size_t savedBytes=prefs.getBytesLength("last");
  const bool legacy=savedBytes==3u*sizeof(uint32_t);
  const bool version2=savedBytes==sizeof(record);
  if ((legacy || version2) &&
      prefs.getBytes("last",record,savedBytes)==savedBytes &&
      (legacy?record[0]==1u:record[0]==2u)) {
    odysseyPersistedRecordStage=uint8_t(record[1]&255u);
    odysseyPersistedRecordBytes=record[2];
    if (version2) {
      // Read the temporary seven-word journal produced during development.
      odysseyPersistedWriteErrno=record[3];
      odysseyPersistedWriteReturned=record[4];
      odysseyPersistedWriteExpected=record[5];
      odysseyPersistedWriteFerror=record[6];
    } else {
      // Keep "last" in its original three-word form so an OTA rollback
      // still sees an unresolved stage-70 failure and retains sleep safety.
      // Extra fields live in a separate stage/byte-paired journal.
      uint32_t detail[7]{};
      const size_t detailLength=prefs.getBytesLength("write");
      if ((detailLength==6u*sizeof(uint32_t) || detailLength==sizeof(detail)) &&
          prefs.getBytes("write",detail,detailLength)==detailLength &&
          detail[0]==record[1] && detail[1]==record[2]) {
        odysseyPersistedWriteErrno=detail[2];
        odysseyPersistedWriteReturned=detail[3];
        odysseyPersistedWriteExpected=detail[4];
        odysseyPersistedWriteFerror=detail[5];
        if (detailLength==sizeof(detail)) odysseyPersistedDriverFault=detail[6];
      }
    }
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
uint32_t odysseyLastWriteErrno() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteErrno.load(); }
uint32_t odysseyLastWriteReturned() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteReturned.load(); }
uint32_t odysseyLastWriteExpected() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteExpected.load(); }
uint32_t odysseyLastWriteFerror() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteFerror.load(); }
uint32_t odysseyLastDriverWriteFault() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedDriverFault.load(); }
static void odysseyPersistRecordFailure(uint8_t stage,uint32_t bytes) {
  odysseyPersistedRecordLoaded=true;
  odysseyPersistedRecordStage=stage;
  odysseyPersistedRecordBytes=bytes;
  if (!stage) {
    odysseyPersistedWriteErrno=0;
    odysseyPersistedWriteReturned=0;
    odysseyPersistedWriteExpected=0;
    odysseyPersistedWriteFerror=0;
    odysseyPersistedDriverFault=0;
  }
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",false)) return;
  // Preserve the original on-flash ABI for older OTA rollback builds.
  // Write optional detail first; last/3-word remains the authoritative stage.
  if (stage) {
    const uint32_t detail[7]={uint32_t(stage),bytes,
      odysseyPersistedWriteErrno.load(),odysseyPersistedWriteReturned.load(),
      odysseyPersistedWriteExpected.load(),odysseyPersistedWriteFerror.load(),
      odysseyPersistedDriverFault.load()};
    (void)prefs.putBytes("write",detail,sizeof(detail));
  }
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
static uint8_t odysseyBatchWriteFailureStage(int error) {
  switch (error) {
    case EIO: return 66;
    case ENODEV: return 67;
    case ENOSPC: return 68;
    case EROFS: return 69;
    default: return 70;
  }
}
// A POSIX fsync on the pinned ESP-IDF FatFs VFS maps to f_sync(), which
// commits file size, allocation and directory metadata while keeping this
// sequential append-only WAV open. The transfer path synthesizes its WAV
// header from the resulting file length after an interrupted power cycle.
// Only invoke after a successful, complete 4 KiB batch under OdysseySdGuard.
static constexpr uint32_t ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u;
static uint8_t odysseyCheckpointWav(FILE* file,int& savedErrno) {
  savedErrno=0;
  errno=0;
  if (fflush(file)!=0) {
    savedErrno=errno?errno:EIO;
    return 71;  // C stdio flush failed
  }
  errno=0;
  const int fd=fileno(file);
  if (fd<0) {
    savedErrno=errno?errno:EBADF;
    return 73;  // no valid VFS descriptor; do not fsync arbitrary fd
  }
  errno=0;
  if (fsync(fd)!=0) {
    savedErrno=errno?errno:EIO;
    return 72;  // FatFs f_sync failed: directory/size durability unknown
  }
  return 0;
}

// Do not trust a single high ADC outlier; two consecutive fresh readings
// must be valid and above the start threshold before any SD recovery/write.
static bool odysseySdPreflightWritePower() {
  sampleBattery(true);
  const bool first=odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV);
  vTaskDelay(pdMS_TO_TICKS(80));
  sampleBattery(true);
  const bool second=odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV);
  if (!first || !second) {
    Serial.printf("[SD] offline write blocked: low/untrusted supply cell=%u available=%u threshold=%u\n",
      unsigned(batteryMillivolts),batteryAvailable.load()?1u:0u,
      unsigned(ODYSSEY_SD_WRITE_START_MIN_MV));
  }
  return first && second;
}

static void odysseyRecordTake() {
  synapSdClearWriteFaultCode();
  odysseyPersistedDriverFault=0;
  odysseyPersistedWriteErrno=0;
  odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=0;
  odysseyPersistedWriteFerror=0;
  bool failed=false;
  uint8_t failureStage=0;
  uint32_t bytes=0;
  uint32_t lastCheckpointAt=millis();
  uint32_t checkpointedPcmBytes=0;
  uint32_t lastPowerSampleAt=millis();
  uint8_t weakPowerSamples=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  // Keep the batch out of the 8 KiB recorder task stack. A 4 KiB aligned
  // write gives FatFs eight contiguous sectors, routing Arduino SD through
  // CMD25 + ACMD23 instead of the fragile one-sector CMD24 loop.
  alignas(4) static uint8_t batch[4096];
  size_t batchUsed=0;
  uint32_t batchPcmBytes=0;
  FILE* file=nullptr;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) { failed=true;failureStage=40; }

  if (!failed) {
    // Append-only recording: create a normal zero-length file and let each
    // successful 4 KiB CMD25 batch extend it naturally. No preallocation means
    // STOP never needs to truncate a 32 MiB logical extent.
    snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
      static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
      failed=true;failureStage=41;
    } else {
      errno=0;
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
    memcpy(batch,header,sizeof(header));
    batchUsed=sizeof(header);
    batchPcmBytes=0;
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
        const size_t room=sizeof(batch)-batchUsed;
        const size_t take=remaining<room?remaining:room;
        memcpy(batch+batchUsed,input,take);
        batchUsed+=take;
        batchPcmBytes+=uint32_t(take);
        input+=take;
        remaining-=take;
        if (batchUsed==sizeof(batch)) {
          errno=0;
          const size_t written=fwrite(batch,1,sizeof(batch),file);
          if (written!=sizeof(batch)) {
            const int writeError=errno;
            odysseyPersistedWriteErrno=writeError>0?uint32_t(writeError):0u;
            odysseyPersistedWriteReturned=uint32_t(written);
            odysseyPersistedWriteExpected=sizeof(batch);
            odysseyPersistedWriteFerror=ferror(file)?1u:0u;
            odysseyPersistedDriverFault=synapSdWriteFaultCode();
            failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
            Serial.printf("[SD] PCM batch write failed errno=%d stage=%u wrote=%u pcm=%lu\n",
              writeError,unsigned(failureStage),unsigned(written),
              static_cast<unsigned long>(bytes));
            break;
          }
          bytes+=batchPcmBytes;
          batchUsed=0;
          batchPcmBytes=0;
          // Recheck Rev K battery while the SD is active; two weak/invalid
          // consecutive samples trigger an orderly STOP before further PCM.
          if (uint32_t(millis()-lastPowerSampleAt)>=5000u) {
            sampleBattery(true);
            lastPowerSampleAt=millis();
            if (!odysseySdPowerSafe(ODYSSEY_SD_WRITE_CONTINUE_MIN_MV)) {
              if (weakPowerSamples<2) ++weakPowerSamples;
            } else weakPowerSamples=0;
            if (weakPowerSamples>=2) {
              Serial.printf("[SD] stopping offline take: voltage no longer safe cell=%u available=%u\n",
                unsigned(batteryMillivolts),batteryAvailable.load()?1u:0u);
              odysseyStopRequested=true;
            }
          }
          // Only checkpoint on a complete batch. FAT's file length is not
          // guaranteed durable until f_sync; a power cut may lose all data
          // since the previous successful checkpoint.
          if (uint32_t(millis()-lastCheckpointAt)>=ODYSSEY_SD_CHECKPOINT_INTERVAL_MS) {
            int checkpointErrno=0;
            const uint8_t checkpointStage=odysseyCheckpointWav(file,checkpointErrno);
            if (checkpointStage) {
              odysseyPersistedWriteErrno=uint32_t(checkpointErrno);
              odysseyPersistedWriteReturned=0;
              odysseyPersistedWriteExpected=0;
              odysseyPersistedWriteFerror=ferror(file)?1u:0u;
              odysseyPersistedDriverFault=synapSdWriteFaultCode();
              failed=true;
              failureStage=checkpointStage;
              Serial.printf("[SD] WAV checkpoint failed stage=%u errno=%d pcm=%lu lastSynced=%lu\n",
                unsigned(checkpointStage),checkpointErrno,
                static_cast<unsigned long>(bytes),
                static_cast<unsigned long>(checkpointedPcmBytes));
              break;
            }
            checkpointedPcmBytes=bytes;
            lastCheckpointAt=millis();
          }
        }
      }

      // Capture performs only sequential 4 KiB writes. Each full batch is
      // sector aligned and FatFs issues a multi-sector disk_write (CMD25).
      // The first batch carries a provisional WAV header; no random rewrite
      // is attempted while the card is in this recording session.
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=43;
#endif

  if (file) {
    if (!failed && batchUsed) {
      // Preserve the multi-block path through STOP: pad the tail to one full
      // 4 KiB batch. This adds at most 128 ms of trailing silence and avoids a
      // final single-sector CMD24 write.
      memset(batch+batchUsed,0,sizeof(batch)-batchUsed);
      errno=0;
      const size_t written=fwrite(batch,1,sizeof(batch),file);
      if (written!=sizeof(batch)) {
        const int writeError=errno;
        odysseyPersistedWriteErrno=writeError>0?uint32_t(writeError):0u;
        odysseyPersistedWriteReturned=uint32_t(written);
        odysseyPersistedWriteExpected=sizeof(batch);
        odysseyPersistedWriteFerror=ferror(file)?1u:0u;
            odysseyPersistedDriverFault=synapSdWriteFaultCode();
        failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
        Serial.printf("[SD] final PCM batch failed errno=%d stage=%u wrote=%u expected=%u\n",
          writeError,unsigned(failureStage),unsigned(written),unsigned(sizeof(batch)));
      } else {
        bytes+=batchPcmBytes;
        batchUsed=0;
        batchPcmBytes=0;
      }
    }

    // STOP is append-only. Periodic f_sync checkpoints plus fclose() make
    // directory size and data recoverable when the filesystem remains readable.
    // The on-card header stays provisional; transfer synthesizes the true WAV
    // header from file size after a clean stop OR interrupted power cycle.
    if (fclose(file)!=0) {
      failed=true;
      if (!failureStage) failureStage=47;
    }
    file=nullptr;
  }

  if (!failed && bytes==0) failureStage=48;
  Serial.printf("[SD] local audio %s: %s, %lu committed PCM bytes stage=%u%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),
    unsigned(failureStage),failed?" (mount retained for recovery)":"");
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
  // An unsafe supply must not provoke a disk remount or the very first
  // CMD24/FAT metadata write. Keep healthy SD files readable from the PWA.
  if (!odysseySdPreflightWritePower()) {
    odysseyRecordFaultAt=millis();
    const uint32_t finalizedAt=millis();
    odysseySdSleepGuardUntil=finalizedAt+5000u;
    disconnectedAt=finalizedAt;
    odysseyRecording=false;
    odysseyStopRequested=false;
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    vTaskDelete(nullptr);
    return;
  }
  // One physical double-tap owns recovery + recording. If the previous take
  // left storage unavailable, recover it here after leaving the touch/control
  // task rather than forcing the user to perform a separate recovery gesture.
  if (!odysseySdReady()) {
    Serial.println("[SD] one-gesture offline start: recovering storage before capture");
    if (!odysseyRecoverSdCard("touch")) {
      odysseyRecordFaultAt=millis();
      // Keep the C3 awake through recovery teardown and a short SD settle.
      // The record-active flag is the sleep veto until cleanup has finished.
      const uint32_t finalizedAt=millis();
      odysseySdSleepGuardUntil=finalizedAt+5000u;
      disconnectedAt=finalizedAt;
      odysseyRecording=false;
      odysseyStopRequested=false;
      applyCpuPowerProfile(false);
      updateStatusLed(true);
      Serial.println("[SD] touch recovery failed; offline recording not started");
      vTaskDelete(nullptr);
      return;
    }
  }

  odysseyRecordTake();

  // A disk error marks the live mount unavailable. The take's guard has
  // unwound at this point, so perform one bounded re-arm before returning to
  // idle. Preserve the recorder failure stage/bytes for diagnostics.
  const uint32_t driverFault=odysseyLastDriverWriteFault();
  const uint8_t driverCommand=uint8_t(driverFault>>24);
  const uint8_t driverPhase=uint8_t(driverFault>>16);
  const bool stuckBusyWrite=
    (driverCommand==24u && driverPhase==4u) ||
    (driverCommand==25u && (driverPhase==4u || driverPhase==6u || driverPhase==7u));
  if (!odysseySdReady() && odysseyLastRecordFailureStage() && !stuckBusyWrite &&
      odysseySdPowerSafe(ODYSSEY_SD_WRITE_START_MIN_MV)) {
    Serial.println("[SD] post-record failure: re-arming storage for next take");
    (void)odysseyRecoverSdCard("rearm");
  } else if (!odysseySdReady() && stuckBusyWrite) {
    // A card held busy after accepting a sector must NOT be hammered with
    // CMD0/CMD12/repeated SD.begin while it still has power.
    odysseySdUnsafeToSleep=true;
    Serial.printf("[SD] held-busy write fault 0x%08lx; suppress auto rearm until power-cycle\n",
      static_cast<unsigned long>(driverFault));
  }
  // An unresolved write/close failure could leave an always-powered SD card
  // inside CMD25 programming. Never allow a later idle timeout or user hold
  // to sleep based solely on the failed mount's "not ready" state.
  const uint8_t lastFault=odysseyLastRecordFailureStage();
  if (!odysseySdReady() && lastFault>=44u && lastFault!=48u) {
    odysseySdUnsafeToSleep=true;
    Serial.printf("[POWER] C3 SD sleep inhibited: unrecovered record stage=%u\n",unsigned(lastFault));
  }

  // Do not lift the sleep veto until fwrite, fclose, persistent diagnostics
  // and any bounded storage re-arm have returned and SD locks were released.
  const uint32_t finalizedAt=millis();
  odysseySdSleepGuardUntil=finalizedAt+5000u;
  disconnectedAt=finalizedAt;
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
  if (deviceConnected.load() || streamingEnabled.load() || OdysseyWifi::busy() ||
      otaBusy() || sleepPending || batteryCritical()) return;
  if (!odysseySdReady()) {
    Serial.println("[TOUCH] SD unavailable; recording task will recover before capture");
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
