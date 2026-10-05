// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <unistd.h>

static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=8192u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_SECTOR_BYTES=512u;
static constexpr size_t ODYSSEY_WAV_HEADER_BYTES=44u;
static constexpr uint32_t ODYSSEY_WAV_CHECKPOINT_MS=15000u;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_SECONDS=300u;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_FRAMES=(SAMPLE_RATE*ODYSSEY_WAV_SEGMENT_SECONDS)/SAMPLES_PER_FRAME;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_PCM_BYTES=ODYSSEY_WAV_SEGMENT_FRAMES*SAMPLES_PER_FRAME*2u;
static_assert(ODYSSEY_WAV_SEGMENT_PCM_BYTES==ODYSSEY_WAV_MAX_PCM_BYTES,
  "inline recovery journal offset must match the five-minute PCM reservation");
static constexpr uint64_t ODYSSEY_WAV_SEGMENT_FILE_BYTES=
  uint64_t(ODYSSEY_INLINE_JOURNAL_OFFSET)+ODYSSEY_INLINE_JOURNAL_BYTES;
static constexpr uint64_t ODYSSEY_SD_FREE_RESERVE_BYTES=2ull*1024ull*1024ull;
static_assert((SAMPLE_RATE*ODYSSEY_WAV_SEGMENT_SECONDS)%SAMPLES_PER_FRAME==0,
  "WAV rollover must align with complete microphone frames");
static uint8_t odysseySdWriteBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];
static std::atomic<uint8_t> odysseyRecordFailureStage{0};
static std::atomic<uint32_t> odysseyRecordLastBytes{0};

uint8_t odysseySdRecordFailureStage() { return odysseyRecordFailureStage.load(); }
uint32_t odysseySdRecordLastBytes() { return odysseyRecordLastBytes.load(); }

static bool odysseyHasSpaceForSegment() {
  const uint64_t freeBytes=odysseySdFreeBytesLocked();
  const uint64_t needed=ODYSSEY_WAV_SEGMENT_FILE_BYTES+ODYSSEY_SD_FREE_RESERVE_BYTES;
  if (freeBytes>=needed) return true;
  errno=ENOSPC;
  Serial.printf("[SD] offline start/rollover refused: free=%llu need=%llu bytes\n",
    static_cast<unsigned long long>(freeBytes),static_cast<unsigned long long>(needed));
  return false;
}

static bool odysseyWriteBufferedChunk(int file,uint8_t* buffer,size_t& buffered,
    size_t count,uint32_t& bytes) {
  if (!count || count>buffered) return count==0;
  size_t completed=0;
  while (completed<count) {
    const ssize_t n=write(file,buffer+completed,count-completed);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) { if (!n) errno=EIO;break; }
    completed+=size_t(n);
  }
  bytes+=uint32_t(completed);buffered-=completed;
  if (buffered && completed) memmove(buffer,buffer+completed,buffered);
  return completed==count;
}

static bool odysseyDrainPcmBuffer(int file,uint8_t* buffer,size_t& buffered,
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

static bool odysseyFinalizeWav(int file,int journal,const char* path,uint32_t& sequence,
    uint8_t* header,uint8_t* buffer,size_t& buffered,uint32_t& bytes) {
  if (!odysseyDrainPcmBuffer(file,buffer,buffered,bytes,true) ||
      !odysseyJournalCommit(file,journal,path,bytes,sequence)) return false;
  // Header is overwritten only when sealing a part. The journal can repair an
  // interrupted header update; it cannot make SD hardware power-loss atomic.
  odysseyWavHeader(header,bytes);
  return odysseyPwriteAll(file,header,ODYSSEY_WAV_HEADER_BYTES,0) && fsync(file)==0 &&
    ftruncate(file,off_t(ODYSSEY_WAV_HEADER_BYTES+bytes))==0 && fsync(file)==0;
}

static void odysseyRecordTake() {
  bool failed=false,storageFailed=false;
  uint8_t failureStage=0;
  int firstErrno=0;
  auto failure=[&](uint8_t stage) -> uint8_t {
    if (!failureStage) firstErrno=errno;
    return failureStage?failureStage:stage;
  };
  uint64_t totalBytes=0;
  uint32_t bytes=0,segment=0;
  uint32_t takeHigh=0,takeLow=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  int file=-1;
  int journal=ODYSSEY_INLINE_JOURNAL;
  uint32_t journalSequence=0;
  uint32_t segmentCrcState=0xffffffffu;
  size_t bufferedBytes=0;

  odysseyRecordFailureStage=0;
  odysseyRecordLastBytes=0;
  errno=0;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage) {
    errno=EBUSY;failed=true;storageFailed=true;failureStage=failure(1);
  } else if (!odysseySdReady()) {
    errno=ENODEV;failed=true;storageFailed=true;failureStage=failure(1);
  } else {
    errno=0;
    if (!odysseySdCloseReadLocked()) {
      failed=true;storageFailed=true;failureStage=failure(1);
    }
  }

  if (!failed) {
    takeHigh=esp_random();takeLow=esp_random();
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx_p%04lu.wav",
        static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
        static_cast<unsigned long>(segment));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
        failed=true;storageFailed=true;failureStage=failure(2);break;
      }
      struct stat existing{};
      errno=0;
      if (stat(fullPath,&existing)==0) {
        // A collision is improbable; regenerate both take identifiers before
        // opening anything so later parts remain a contiguous numbered set.
        takeHigh=esp_random();takeLow=esp_random();
        continue;
      }
      if (errno!=ENOENT) { failed=true;storageFailed=true;failureStage=failure(2);break; }
      if (!odysseyHasSpaceForSegment()) {
        failed=true;failureStage=failure(31);break;
      }
      if (!odysseySdPreallocateFile(fullPath,ODYSSEY_WAV_SEGMENT_FILE_BYTES)) {
        failed=true;storageFailed=true;failureStage=failure(30);break;
      }
      errno=0;
      file=open(fullPath,O_RDWR);
      if (file>=0) break;
      failed=true;storageFailed=true;failureStage=failure(3);break;
    }
    if (file<0 && !failed) { failed=true;storageFailed=true;failureStage=failure(2); }
  }

  odysseyWavHeader(header,0);
  if (!failed) {
    errno=0;
    if (!odysseyPwriteAll(file,header,sizeof(header),0)) failureStage=failure(33);
    else if (lseek(file,ODYSSEY_WAV_HEADER_BYTES,SEEK_SET)<0) failureStage=failure(34);
    else if (!odysseyJournalCommit(file,ODYSSEY_INLINE_JOURNAL,fullPath,0,journalSequence))
      failureStage=failure(35);
    if (failureStage) { failed=true;storageFailed=true; }
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    errno=0;
    if (!startMicrophone()) { failed=true;failureStage=failure(4); }
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    uint32_t checkpointAt=millis();
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true;failureStage=failure(4);break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (uint64_t(bytes)+uint64_t(bufferedBytes)+sizeof(pcm)>ODYSSEY_WAV_SEGMENT_PCM_BYTES) {
        failed=true;storageFailed=true;failureStage=failure(5);break;
      }

      // Open the next reserved part only after a complete PCM frame has been
      // captured. A stop exactly at a segment boundary then leaves no empty
      // next-part file on the card.
      if (file<0) {
        snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx_p%04lu.wav",
          static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
          static_cast<unsigned long>(segment));
        if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
          failed=true;storageFailed=true;failureStage=failure(2);break;
        }
        if (!odysseyHasSpaceForSegment()) {
          failed=true;failureStage=failure(31);break;
        }
        if (!odysseySdPreallocateFile(fullPath,ODYSSEY_WAV_SEGMENT_FILE_BYTES)) {
          failed=true;storageFailed=true;failureStage=failure(30);break;
        }
        errno=0;
        file=open(fullPath,O_RDWR);
        if (file<0) {
          failed=true;storageFailed=true;failureStage=failure(3);break;
        }
        journal=ODYSSEY_INLINE_JOURNAL;journalSequence=0;
        bytes=0;bufferedBytes=0;segmentCrcState=0xffffffffu;odysseyWavHeader(header,0);
        if (!odysseyPwriteAll(file,header,sizeof(header),0)) {
          failed=true;storageFailed=true;failureStage=failure(33);break;
        }
        if (lseek(file,ODYSSEY_WAV_HEADER_BYTES,SEEK_SET)<0) {
          failed=true;storageFailed=true;failureStage=failure(34);break;
        }
        if (!odysseyJournalCommit(file,journal,fullPath,0,journalSequence)) {
          failed=true;storageFailed=true;failureStage=failure(35);break;
        }
        checkpointAt=millis();
      }

      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=failure(5);break;
        }
      }
      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        failed=true;failureStage=failure(5);break;
      }
      memcpy(odysseySdWriteBuffer+bufferedBytes,pcm,sizeof(pcm));
      bufferedBytes+=sizeof(pcm);
      segmentCrcState=odysseySdCrcUpdate(segmentCrcState,
        reinterpret_cast<const uint8_t*>(pcm),sizeof(pcm));

      if (bufferedBytes>=ODYSSEY_SD_WRITE_CHUNK_BYTES+ODYSSEY_SD_SECTOR_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=failure(5);break;
        }
      }

      // Commit progress without repeatedly overwriting the WAV header sector.
      if (uint32_t(millis()-checkpointAt)>=ODYSSEY_WAV_CHECKPOINT_MS) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false) ||
            !odysseyJournalCommit(file,journal,fullPath,bytes,journalSequence)) {
          failed=true;storageFailed=true;failureStage=failure(6);break;
        }
        checkpointAt=millis();
      }

      if (uint64_t(bytes)+bufferedBytes==ODYSSEY_WAV_SEGMENT_PCM_BYTES) {
        if (!odysseyFinalizeWav(file,journal,fullPath,journalSequence,header,odysseySdWriteBuffer,bufferedBytes,bytes)) {
          failed=true;storageFailed=true;failureStage=failure(6);break;
        }
        const uint32_t completedBytes=bytes;
        const uint32_t completedCrc=~segmentCrcState;
        totalBytes+=completedBytes;
        bytes=0;
        const int completed=file;file=-1;
        int closeError=0;
        errno=0;
        if (close(completed)!=0) closeError=errno?errno:EIO;
        journal=ODYSSEY_INLINE_JOURNAL;
        if (closeError) {
          errno=closeError;
          failed=true;storageFailed=true;failureStage=failure(7);break;
        }
        if (!odysseyWriteWavMeta(fullPath,takeHigh,takeLow,segment,completedBytes,completedCrc))
          Serial.printf("[SD] metadata sidecar deferred for part %lu errno=%d\n",
            static_cast<unsigned long>(segment),errno);
        ++segment;
        segmentCrcState=0xffffffffu;
      }
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=failure(4);
#endif

  if (file>=0) {
    // Do not retry writes or rewrite the header after FatFs reports a storage
    // error: a failed FatFs file object may be aborted, and the seek position
    // may no longer be trustworthy. Close is still attempted for cleanup.
    if (!storageFailed) {
      if (!odysseyFinalizeWav(file,journal,fullPath,journalSequence,header,odysseySdWriteBuffer,bufferedBytes,bytes)) {
        failed=true;storageFailed=true;if (!failureStage) failureStage=failure(6);
      }
    }
    totalBytes+=bytes;
    errno=0;
    if (close(file)!=0) {
      const int closeError=errno?errno:EIO;
      failed=true;storageFailed=true;
      if (!failureStage) { errno=closeError;failureStage=failure(7); }
    }
    file=-1;
  }

  // New recordings keep recovery commits inside the WAV reservation, so there
  // is no second descriptor to close and no .jrn unlink on the clean path.
  journal=ODYSSEY_INLINE_JOURNAL;

  if (!storageFailed && bytes && fullPath[0]) {
    const uint32_t finalCrc=~segmentCrcState;
    if (!odysseyWriteWavMeta(fullPath,takeHigh,takeLow,segment,bytes,finalCrc))
      Serial.printf("[SD] metadata sidecar deferred for final part %lu errno=%d\n",
        static_cast<unsigned long>(segment),errno);
  }

  // A user can stop before the first complete PCM frame, and I2S can fail at
  // startup. Do not leave an empty, otherwise-valid WAV in the sync catalogue.
  if (!storageFailed && totalBytes==0 && fullPath[0] && unlink(fullPath)!=0 && errno!=ENOENT) {
    failed=true;storageFailed=true;if (!failureStage) failureStage=failure(7);
  }

  if (!failed && totalBytes==0) failureStage=failure(8);
  odysseyRecordFailureStage=failureStage;
  odysseyRecordLastBytes=uint32_t(std::min<uint64_t>(totalBytes,0xffffffffull));

  Serial.printf("[SD] local audio %s: %08lx_%08lx, %llu PCM bytes, segments=%lu stage=%u buffer=%u chunk=%u segment=%lus checkpoint=%lus%s\n",
    failed?"failed":"saved",static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
    static_cast<unsigned long long>(totalBytes),static_cast<unsigned long>(segment+1),unsigned(failureStage),
    unsigned(ODYSSEY_SD_WRITE_BUFFER_BYTES),unsigned(ODYSSEY_SD_WRITE_CHUNK_BYTES),
    static_cast<unsigned long>(ODYSSEY_WAV_SEGMENT_SECONDS),
    static_cast<unsigned long>(ODYSSEY_WAV_CHECKPOINT_MS/1000u),
    failed?" (mount retained for explicit recovery)":"");
  if (failed) odysseySaveRecordFailure(failureStage,firstErrno,odysseyRecordLastBytes.load());
  else if (totalBytes) odysseySaveRecordFailure(0,0,0);
  if (storageFailed) { errno=firstErrno?firstErrno:EIO;odysseySdMarkVfsFailure(); }
  if (failed || totalBytes==0) odysseyRecordFaultAt=millis();
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  bool ready=odysseySdReady();
  bool captureAttempted=false,startRecoveryAttempted=false;
  if (!ready && !odysseyStopRequested.load()) {
    startRecoveryAttempted=true;
    odysseySdRecoveryActive=true;
    updateStatusLed(true);
    Serial.println("[SD] one-gesture offline start: recovering storage before capture");
    ready=odysseyRecoverSdCard("touch");
    odysseySdRecoveryActive=false;
    if (ready) {
      odysseyRecordingStartedAt=millis();
      updateStatusLed(true);
      Serial.println("[SD] offline recovery succeeded; capture starting from original double tap");
    }
  }

  if (ready && !odysseyStopRequested.load()) {
    captureAttempted=true;
    odysseyRecordTake();
  } else if (!ready) {
    errno=ENODEV;
    odysseyRecordFailureStage=1;
    odysseyRecordLastBytes=0;
    odysseySaveRecordFailure(1,ENODEV,0);
    odysseyRecordFaultAt=millis();
    Serial.println("[SD] offline recovery failed; recording did not start");
  }

  // After checked fsync/close returns, give the SD card time to finish any
  // internal flash programming before power management is allowed to tear
  // down the SPI host. This also resets the disconnected idle window after
  // every offline take instead of inheriting a stale BLE disconnect timestamp.
  const uint32_t finalizedAt=millis();
  odysseySdSleepGuardUntil=finalizedAt+5000u;
  disconnectedAt=finalizedAt;
  odysseyRecording=false;
  odysseyStopRequested=false;
  applyCpuPowerProfile(false);
  updateStatusLed(true);

  // A media I/O failure invalidates the current VFS, but a disconnected device
  // must not depend on the phone for recovery. Prepare the next take only; never
  // append to or retry the failed WAV.
  if (captureAttempted && !startRecoveryAttempted && !odysseySdReady() &&
      !deviceConnected.load() && !streamingEnabled.load() &&
      !otaBusy() && !sleepPending && !batteryCritical()) {
    odysseySdRecoveryActive=true;
    updateStatusLed(true);
    delay(750u);
    Serial.println("[SD] autonomous post-record recovery");
    (void)odysseyRecoverSdCard("post-record");
    odysseySdRecoveryActive=false;
    updateStatusLed(true);
  }
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
  if (odysseySdRecoveryActive.load()) return;
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;
  odysseyStopRequested=false;
  odysseyRecordingStartedAt=millis();
  odysseyRecordFaultAt=0;
  odysseyRecording=true;
  if (!odysseySdReady()) odysseySdRecoveryActive=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseyStopRequested=false;
    odysseySdRecoveryActive=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println(odysseySdReady()?
    "[TOUCH] double tap -> SD audio START":
    "[TOUCH] double tap -> SD recover + audio START");
}
#endif
