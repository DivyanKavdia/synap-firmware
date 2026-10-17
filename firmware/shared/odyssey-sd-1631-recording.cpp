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
// New diagnostic journal does not change the existing last/write ABI.
static std::atomic<uint32_t> odysseyPersistedWireCmd{UINT32_MAX};
static std::atomic<uint32_t> odysseyPersistedWireErr{0};
static std::atomic<uint32_t> odysseyPersistedWireArg{0};
static std::atomic<uint32_t> odysseyPersistedWireRsp{0};
static std::atomic<uint32_t> odysseyPersistedWireCmd13{0};
// Additive optional NVS 'wtime' journal; keep old last/write/wire ABI intact.
static std::atomic<uint32_t> odysseyPersistedWireMs{0};
static std::atomic<uint32_t> odysseyPersistedWireBudget{0};
static std::atomic<uint32_t> odysseyPersistedWireLen{0};
static std::atomic<uint32_t> odysseyPersistedWirePrev{UINT32_MAX};
static std::atomic<uint32_t> odysseyPersistedWireGoodWrites{0};
void odysseySdClearWireFault();
uint32_t odysseySdWireFaultCmd();
uint32_t odysseySdWireFaultError();
uint32_t odysseySdWireFaultArg();
uint32_t odysseySdWireFaultResponse();
uint32_t odysseySdLastCmd13Response();
uint32_t odysseySdWireFaultElapsedMs();
uint32_t odysseySdWireFaultBudgetMs();
uint32_t odysseySdWireFaultLength();
uint32_t odysseySdWireFaultPreviousCmd();
uint32_t odysseySdWireFaultGoodWrites();
extern "C" uint32_t synapSdWriteFaultCode();
extern "C" void synapSdClearWriteFaultCode();
void synapSdNoteNativeWriteError(uint8_t stage,int ioError);

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
    // Only restore command diagnostics when stage AND accepted PCM bytes
    // match the authoritative older recorder journal; never mix attempts.
    uint32_t wire[7]{};
    if (prefs.getBytesLength("wire")==sizeof(wire) &&
        prefs.getBytes("wire",wire,sizeof(wire))==sizeof(wire) &&
        wire[0]==record[1] && wire[1]==record[2]) {
      odysseyPersistedWireCmd=wire[2];
      odysseyPersistedWireErr=wire[3];
      odysseyPersistedWireArg=wire[4];
      odysseyPersistedWireRsp=wire[5];
      odysseyPersistedWireCmd13=wire[6];
    }
    uint32_t timing[7]{};
    if (prefs.getBytesLength("wtime")==sizeof(timing) &&
        prefs.getBytes("wtime",timing,sizeof(timing))==sizeof(timing) &&
        timing[0]==record[1] && timing[1]==record[2]) {
      odysseyPersistedWireMs=timing[2];
      odysseyPersistedWireBudget=timing[3];
      odysseyPersistedWireLen=timing[4];
      odysseyPersistedWirePrev=timing[5];
      odysseyPersistedWireGoodWrites=timing[6];
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
uint32_t odysseyLastWireCommand() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireCmd.load(); }
uint32_t odysseyLastWireError() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireErr.load(); }
uint32_t odysseyLastWireArgument() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireArg.load(); }
uint32_t odysseyLastWireResponse() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireRsp.load(); }
uint32_t odysseyLastWireCmd13() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireCmd13.load(); }
uint32_t odysseyLastWireMs() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireMs.load(); }
uint32_t odysseyLastWireBudget() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireBudget.load(); }
uint32_t odysseyLastWireLen() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireLen.load(); }
uint32_t odysseyLastWirePrev() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWirePrev.load(); }
uint32_t odysseyLastWireGoodWrites() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWireGoodWrites.load(); }
static void odysseyPersistRecordFailure(uint8_t stage,uint32_t bytes) {
  odysseyPersistedRecordLoaded=true;
  odysseyPersistedRecordStage=stage;
  odysseyPersistedRecordBytes=bytes;
  odysseyPersistedWireCmd=stage?odysseySdWireFaultCmd():UINT32_MAX;
  odysseyPersistedWireErr=stage?odysseySdWireFaultError():0u;
  odysseyPersistedWireArg=stage?odysseySdWireFaultArg():0u;
  odysseyPersistedWireRsp=stage?odysseySdWireFaultResponse():0u;
  odysseyPersistedWireCmd13=stage?odysseySdLastCmd13Response():0u;
  odysseyPersistedWireMs=stage?odysseySdWireFaultElapsedMs():0u;
  odysseyPersistedWireBudget=stage?odysseySdWireFaultBudgetMs():0u;
  odysseyPersistedWireLen=stage?odysseySdWireFaultLength():0u;
  odysseyPersistedWirePrev=stage?odysseySdWireFaultPreviousCmd():UINT32_MAX;
  odysseyPersistedWireGoodWrites=stage?odysseySdWireFaultGoodWrites():0u;
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
    const uint32_t wire[7]={uint32_t(stage),bytes,
      odysseyPersistedWireCmd.load(),odysseyPersistedWireErr.load(),
      odysseyPersistedWireArg.load(),odysseyPersistedWireRsp.load(),
      odysseyPersistedWireCmd13.load()};
    (void)prefs.putBytes("wire",wire,sizeof(wire));
    const uint32_t timing[7]={uint32_t(stage),bytes,
      odysseyPersistedWireMs.load(),odysseyPersistedWireBudget.load(),
      odysseyPersistedWireLen.load(),odysseyPersistedWirePrev.load(),
      odysseyPersistedWireGoodWrites.load()};
    (void)prefs.putBytes("wtime",timing,sizeof(timing));
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
// Only invoke after a successful, complete aligned PCM batch under OdysseySdGuard.
// Fewer FAT directory/FSInfo CMD24 metadata writes while audio is active.
// Ten-second f_sync was a regular single-sector write stressor on the
// affected C3+SD carrier. This trades a longer unsynced tail (up to ~60 s)
// for substantially fewer controller program-busy transitions.
// An interrupted 20-second take previously looked like 0 B because the
// first metadata sync was deferred until 60 seconds. One early f_sync after
// 256 KiB of successfully appended PCM (~8.2 s at 16 kHz mono PCM16)
// bounds this exposure without reinstating the old every-10-second sync.
// This cannot fix a CMD25 busy timeout and adds one metadata CMD24; trial
// on a disposable card before releasing on the sealed production carrier.
// Experimental C3 compatibility profile: 4 contiguous sectors per CMD25 instead
// of 8. Never fall back to CMD24 or retry an uncertain write.
static constexpr size_t ODYSSEY_SD_RECORD_BATCH_BYTES=2048u;
static_assert(ODYSSEY_SD_RECORD_BATCH_BYTES>=1024u &&
  ODYSSEY_SD_RECORD_BATCH_BYTES%512u==0u,
  "C3 SD recording must use aligned multi-sector CMD25 writes");
static constexpr uint32_t ODYSSEY_SD_FIRST_CHECKPOINT_PCM_BYTES=262144u;
static constexpr uint32_t ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=60000u;
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

// Explicit disconnected double-tap is the sole entry to this recorder.
// The mounted FAT root, the storage mutex and each actual write determine
// success. Do not block an actual SD I/O test on unrelated ADC telemetry.
// Always reject stuck-low MISO, mount failure, short writes and sync errors.
// Save the FIRST low-level CMD24/CMD25 fault even when FatFs aborts inside
// mkdir() or fopen() before the normal audio fwrite() loop is entered.
// A 0 driver fault with EIO points to another FAT/VFS I/O failure.
static void odysseyCaptureCreateFault(int err,const char* operation,uint8_t stage) {
  odysseyPersistedWriteErrno=err>0?uint32_t(err):uint32_t(EIO);
  odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=0;
  odysseyPersistedWriteFerror=0;
  synapSdNoteNativeWriteError(stage,err);
  odysseyPersistedDriverFault=synapSdWriteFaultCode();
  Serial.printf("[SD] %s failed stage=%u errno=%d driverFault=0x%08lx\\n",
    operation,unsigned(stage),err,
    static_cast<unsigned long>(odysseyPersistedDriverFault.load()));
}

static void odysseyRecordTake() {
  odysseySdClearWireFault();  // first ACTUAL CMD fault of this take
  odysseyMicRecoveryAttempts=0;
  synapSdClearWriteFaultCode();
  odysseyPersistedDriverFault=0;
  odysseyPersistedWriteErrno=0;
  odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=0;
  odysseyPersistedWriteFerror=0;
  bool failed=false;
  bool uncertainSdWrite=false; // No FAT close/metadata writes after unknown write EIO
  uint8_t failureStage=0;
  uint32_t bytes=0;
  uint32_t lastCheckpointAt=millis();
  uint32_t checkpointedPcmBytes=0;
  // Recorder progress is determined by SD write results, not ADC telemetry.
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  // Keep the batch out of the 8 KiB recorder task stack. A 2 KiB aligned
  // write gives FatFs four contiguous sectors through the native SDSPI
  // CMD25 path, avoiding fragile single-sector CMD24 while bounding each
  // multi-block program transaction more tightly.
  alignas(4) static uint8_t batch[ODYSSEY_SD_RECORD_BATCH_BYTES];
  size_t batchUsed=0;
  uint32_t batchPcmBytes=0;
  FILE* file=nullptr;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) { failed=true;failureStage=40; }

  if (!failed) {
    // No mkdir, long-filename allocation, preallocation, or root probe writes.
    // New capture files use FAT 8.3 root entries. This removes the precise
    // stage-76 directory creation operation that timed out on the field card.
    // The stable *logical* media path is still /synap/Rxxxxxxx.WAV.
    for(uint8_t attempt=0;attempt<16u && !failed && !file;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/R%07lX.WAV",
        static_cast<unsigned long>(esp_random()&0x0fffffffu));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
        failed=true;failureStage=41;break;
      }
      struct stat exists{};
      errno=0;
      if (stat(fullPath,&exists)==0) continue; // collision; never overwrite
      if (errno!=ENOENT) {
        failed=true;failureStage=77;
        odysseyCaptureCreateFault(errno?errno:EIO,"root WAV stat",failureStage);
        break;
      }
      errno=0;
      file=fopen(fullPath,"wb");
      if (!file) {
        const int openError=errno;
        failed=true;failureStage=odysseyCreateFailureStage(openError);
        odysseyCaptureCreateFault(openError,"root WAV fopen",failureStage);
        Serial.printf("[SD] 8.3 root WAV create failed path=%s\\n",fullPath);
      } else if (setvbuf(file,nullptr,_IONBF,0)!=0) {
        failed=true;failureStage=42;
      }
    }
    if (!failed && !file) { // RNG collision exhausted; do not open/truncate
      failed=true;failureStage=41;
      odysseyCaptureCreateFault(EEXIST,"root WAV random name collisions",failureStage);
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
      uint32_t noAudioSince=millis();
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          // A stalled SDSPI write or task scheduling delay can briefly
          // starve I2S; three zero reads (often <250 ms) were treated as a
          // fatal microphone error. Allow one second before abandoning a take.
          if (uint32_t(millis()-noAudioSince)>=1000u) {
            // Recover one intermittent microphone DMA stall, but never
            // repeat indefinitely or mistake an SD timeout for an I2S fault.
            if (!odysseyMicRecoveryAttempts.load() && !odysseyStopRequested.load()) {
              odysseyMicRecoveryAttempts=1;
              Serial.println("[MIC] local I2S stalled; controlled one-time restart");
              stopMicrophone();
              if (startMicrophone()) {
                received=0;noAudioSince=millis();continue;
              }
            }
            failed=true;failureStage=43;break;
          }
          vTaskDelay(1);
        } else {
          received+=count;
          noAudioSince=millis();
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
            synapSdNoteNativeWriteError(odysseyBatchWriteFailureStage(writeError),writeError);
            odysseyPersistedDriverFault=synapSdWriteFaultCode();
            if (writeError!=ENOSPC) uncertainSdWrite=true;
            failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
            Serial.printf("[SD] PCM batch write failed errno=%d stage=%u wrote=%u pcm=%lu\n",
              writeError,unsigned(failureStage),unsigned(written),
              static_cast<unsigned long>(bytes));
            break;
          }
          bytes+=batchPcmBytes;
          batchUsed=0;
          batchPcmBytes=0;
          // This is an explicit SD write test: trust the actual complete
          // aligned multi-sector batch result, not uncalibrated battery ADC readings.
          // The first 256 KiB and subsequent 60-second checkpoints sync FAT
          // file-length metadata. An unexpected power cut can lose the unsynced tail;
          // FAT sectors in flight can still tear on a sudden rail collapse.
          // The native SDSPI host owns write completion and timeout checks.
          const bool firstCheckpointDue=checkpointedPcmBytes==0u &&
            bytes>=ODYSSEY_SD_FIRST_CHECKPOINT_PCM_BYTES;
          const bool periodicCheckpointDue=
            uint32_t(millis()-lastCheckpointAt)>=ODYSSEY_SD_CHECKPOINT_INTERVAL_MS;
          if (firstCheckpointDue || periodicCheckpointDue) {
            int checkpointErrno=0;
            const uint8_t checkpointStage=odysseyCheckpointWav(file,checkpointErrno);
            if (checkpointStage) {
              odysseyPersistedWriteErrno=uint32_t(checkpointErrno);
              odysseyPersistedWriteReturned=0;
              odysseyPersistedWriteExpected=0;
              odysseyPersistedWriteFerror=ferror(file)?1u:0u;
              synapSdNoteNativeWriteError(checkpointStage,checkpointErrno);
            odysseyPersistedDriverFault=synapSdWriteFaultCode();
              uncertainSdWrite=true;
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
            if (firstCheckpointDue) Serial.printf(
              "[SD] first WAV metadata checkpoint synced pcm=%lu (not proof of card-level persistence)\n",
              static_cast<unsigned long>(bytes));
          }
        }
      }

      // Capture performs only sequential 2 KiB multi-sector writes through native SDSPI.
      // Card command selection and completion belong to ESP-IDF FatFs.
      // The first batch carries a provisional WAV header; no random rewrite
      // is attempted while the card is in this recording session.
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=43;
#endif

  if (file && (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite)) {
    if (uncertainSdWrite) odysseySdQuarantineUncertainWrite();
    // NEVER fclose/f_sync after a timed-out CMD24/CMD25. FatFs fclose can
    // itself write directory/FAT sectors, injecting new commands into a card
    // still asserting BUSY and turning one write failure into permanent loss
    // of SD responsiveness. Leave the descriptor in RAM until a real rail
    // power-cycle/restart; no other FAT access is permitted.
    failed=true;
    if (!failureStage) failureStage=79; // physical program timeout
    if (!odysseyPersistedWriteErrno.load()) odysseyPersistedWriteErrno=EIO;
    odysseySdBootState=2;
    odysseySdProbeStage=failureStage;
    odysseySdUnsafeToSleep=true;
    file=nullptr;
    Serial.println("[SD] uncertain SD write: skip unsafe fclose/FAT updates; preserve WAV prefix for cold recovery");
  }
  if (file) {
    if (!failed && batchUsed) {
      // Preserve the multi-block path through STOP: pad the tail to one full
      // 2 KiB batch. This adds at most 64 ms of trailing silence and avoids a
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
            synapSdNoteNativeWriteError(odysseyBatchWriteFailureStage(writeError),writeError);
            odysseyPersistedDriverFault=synapSdWriteFaultCode();
        if (writeError!=ENOSPC) uncertainSdWrite=true;
        failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
        Serial.printf("[SD] final PCM batch failed errno=%d stage=%u wrote=%u expected=%u\n",
          writeError,unsigned(failureStage),unsigned(written),unsigned(sizeof(batch)));
      } else {
        bytes+=batchPcmBytes;
        batchUsed=0;
        batchPcmBytes=0;
      }
    }

    // STOP is append-only. One early f_sync, then fclose() on STOP minimizes
    // metadata programming during active capture. After power loss only the
    // early checkpointed prefix is guaranteed to be discoverable.
    // The on-card header stays provisional; transfer synthesizes the true WAV
    // header from file size after a clean stop OR interrupted power cycle.
    // The LAST padded 2 KiB write occurs after the earlier timeout gate.
    // It can itself time out inside CMD25/CMD13. Never follow such a failure
    // with fclose(), which may issue another FatFs directory/CMD24 write to
    // a controller that is still busy programming. Keep the VFS descriptor
    // stranded until the genuine SD power cycle; no further SD I/O is safe.
    if (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite) {
      if (uncertainSdWrite) odysseySdQuarantineUncertainWrite();
      failed=true;
      if (!failureStage) failureStage=79;
      if (!odysseyPersistedWriteErrno.load()) odysseyPersistedWriteErrno=EIO;
      odysseySdBootState=2;
      odysseySdProbeStage=failureStage;
      odysseySdUnsafeToSleep=true;
      file=nullptr;
      Serial.println("[SD] final PCM write timed out: skip unsafe fclose; card quarantined");
    } else {
      errno=0;
      if (fclose(file)!=0) {
        const int closeErrno=errno;
        failed=true;
        if (!failureStage) failureStage=47;
        if (!odysseyPersistedWriteErrno.load())
          odysseyCaptureCreateFault(closeErrno,"fclose WAV",failureStage);
      }
      file=nullptr;
    }
  }

  if (!failed && bytes==0) failureStage=48;
  // A successful fwrite/fclose is not proof of persisted FAT data. Re-open
  // the closed WAV and read the physical on-card RIFF header. The PWA will
  // synthesize logical RIFF/data lengths from st_size during BLE sync.
  if (!failed && bytes) {
    struct stat persisted{};
    errno=0;
    if (!fullPath[0] || stat(fullPath,&persisted)!=0 ||
        !S_ISREG(persisted.st_mode) ||
        persisted.st_size<44 ||
        uint64_t(persisted.st_size)<uint64_t(44u)+bytes) {
      failed=true;failureStage=74;
      odysseyPersistedWriteErrno=errno?uint32_t(errno):uint32_t(EIO);
      Serial.printf("[SD] WAV verify stat failed errno=%d pcm=%lu\n",
        int(odysseyPersistedWriteErrno.load()),static_cast<unsigned long>(bytes));
    } else {
      errno=0;
      FILE* verify=fopen(fullPath,"rb");
      uint8_t headerReadback[44]{};
      const size_t got=verify?fread(headerReadback,1,sizeof(headerReadback),verify):0;
      const int readErrno=errno;
      const bool closeOk=verify && fclose(verify)==0;
      if (got!=sizeof(headerReadback) || !closeOk ||
          memcmp(headerReadback,"RIFF",4)!=0 ||
          memcmp(headerReadback+8,"WAVEfmt ",8)!=0 ||
          memcmp(headerReadback+36,"data",4)!=0) {
        failed=true;failureStage=75;
        odysseyPersistedWriteErrno=readErrno>0?uint32_t(readErrno):uint32_t(EIO);
        Serial.printf("[SD] WAV verify readback failed errno=%d got=%u\n",
          int(odysseyPersistedWriteErrno.load()),unsigned(got));
      } else {
        Serial.printf("[SD] WAV VERIFIED logical=%s fileBytes=%llu pcm=%lu\n",
          logicalPath,static_cast<unsigned long long>(persisted.st_size),
          static_cast<unsigned long>(bytes));
      }
    }
  }
  Serial.printf("[SD] local audio %s: %s, %lu committed PCM bytes stage=%u%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),
    unsigned(failureStage),failed?" (mount retained for recovery)":"");
  // Diagnostics are published only after the historical 1631 I/O sequence has
  // completed, so they cannot change the write timing being measured.
  if (failed || bytes==0) {
    const uint8_t persistedStage=failureStage?failureStage:40;
    // Revoke a previous healthy-cold-mount witness before recording any new
    // SD storage failure. Never clear the underlying failure diagnostics.
    if (persistedStage!=43u && persistedStage!=48u)
      odysseySdInvalidateHistoricalFaultResolution();
    odysseyPersistRecordFailure(persistedStage,bytes);
    // Mic failure (43) and an empty, user-stopped take (48) do not establish
    // that the SD card failed. Keep a still-mounted FAT volume accessible so
    // the user can delete incomplete WAVs. Only storage/verify faults revoke
    // SD readiness. Never remove or format a failed recording automatically.
    const bool storageFailure=persistedStage!=43u && persistedStage!=48u &&
      persistedStage!=52u && persistedStage!=68u; // ENOSPC is not a bus fault
    odysseyRecorderFaultLedCode=persistedStage==43u?3u:
      (odysseySdWriteTimeoutQuarantined()?4u:2u);
    if (storageFailure) {
      odysseySdBootState=2;
      odysseySdProbeStage=persistedStage;
      odysseySdUnsafeToSleep=true;
    }
    odysseyRecordFaultAt=millis();
  } else {
    odysseyPersistRecordFailure(0,0);
    odysseyRecorderFaultLedCode=0;
    odysseySdBootState=1;
    odysseySdProbeStage=6;
  }
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  // The user's explicit offline gesture owns this attempt. Actual card
  // readiness and I/O failures are checked below; no ADC admission gate.
  // One physical double-tap owns recovery + recording. If the previous take
  // left storage unavailable, recover it here after leaving the touch/control
  // task rather than forcing the user to perform a separate recovery gesture.
  const bool previousProgramTimeout =
    (odysseyLastWireError()==uint32_t(ESP_ERR_TIMEOUT) ||
     odysseyLastWireError()==uint32_t(ESP_ERR_INVALID_RESPONSE)) &&
    (odysseyLastWireCommand()==24u || odysseyLastWireCommand()==25u ||
      (odysseyLastWireCommand()==13u &&
        (odysseyLastWirePrev()==24u || odysseyLastWirePrev()==25u)));
  if (!odysseySdReady() &&
      (odysseySdWriteTimeoutQuarantined() ||
       (previousProgramTimeout && esp_reset_reason()!=ESP_RST_POWERON))) {
    // Warm reset does not reset the always-powered SD rail. Do not remount
    // after a recorded NAND program timeout; it risks compounding failure.
    const uint32_t finalizedAt=millis();
    odysseyRecordFaultAt=finalizedAt;
    odysseyRecorderFaultLedCode=4;
    odysseySdSleepGuardUntil=finalizedAt+5000u;
    disconnectedAt=finalizedAt;
    odysseyRecording=false;
    odysseyStopRequested=false;
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] offline start refused: prior unsafe CMD24/CMD25 response; power-cycle SD, never automatic remount");
    vTaskDelete(nullptr);
    return;
  }
  if (!odysseySdReady() && odysseySdBusStuckLow()) {
    // A card that drives MISO LOW with CS high cannot recover through
    // repeated CMD0/CMD12 remount attempts on the same powered SD rail.
    // Do not let one double-tap restart an already-known stuck-low card.
    const uint32_t finalizedAt=millis();
    odysseyRecordFaultAt=finalizedAt;
    odysseyRecorderFaultLedCode=4;
    odysseySdSleepGuardUntil=finalizedAt+5000u;
    disconnectedAt=finalizedAt;
    odysseyRecording=false;
    odysseyStopRequested=false;
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] offline start blocked: MISO stuck LOW; full card power-cycle required");
    vTaskDelete(nullptr);
    return;
  }
  if (!odysseySdReady() && odysseySdControllerBusyLow()) {
    // The last passive probe saw CS-high MISO released but a selected
    // controller responding 0x00 continuously. A new mount cannot restore
    // a card stuck internally BUSY. Never create another empty WAV or
    // hammer the continuously-powered SD with CMD0/CMD12.
    const uint32_t finalizedAt=millis();
    odysseyRecordFaultAt=finalizedAt;
    odysseyRecorderFaultLedCode=4;
    odysseySdSleepGuardUntil=finalizedAt+5000u;
    disconnectedAt=finalizedAt;
    odysseyRecording=false;
    odysseyStopRequested=false;
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] offline start blocked: controller busy-low; power-cycle SD rail");
    vTaskDelete(nullptr);
    return;
  }
  if (!odysseySdReady()) {
    Serial.println("[SD] one-gesture offline start: recovering storage before capture");
    if (!odysseyRecoverSdCard("touch")) {
      odysseyRecordFaultAt=millis();
      odysseyRecorderFaultLedCode=4;
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

  // A native write/create/close EIO is a FatFs/VFS errno, NOT an SD
  // wire-level CMD24/CMD25 result. synapSdWriteFaultCode() encodes faults as
  // 0x20SS00EE (marker, stage, errno). The former decoder treated 0x20 as
  // command 32, assumed the card was idle, and immediately issued CMD12/CMD0
  // after a failed write. A card still BUSY/programming may then latch up
  // until its power rail is cycled. Do not re-arm a failed, still-powered card
  // in the recorder task. Keep the FAT volume and every partial take intact.
  // Recovery is explicitly user-initiated (Check SD / a later touch gesture)
  // after the bus has had time to settle, or follows a true card power cycle.
  const uint8_t lastFault=odysseyLastRecordFailureStage();
  if (!odysseySdReady() && lastFault) {
    odysseySdUnsafeToSleep=true;
    Serial.printf("[SD] recorder fault stage=%u io=0x%08lx: skipping automatic remount; retain SD files\n",
      unsigned(lastFault),static_cast<unsigned long>(odysseyLastDriverWriteFault()));
  }
  // An unresolved write/close failure could leave an always-powered SD card
  // inside CMD25 programming. Never allow a later idle timeout or user hold
  // to sleep based solely on the failed mount's "not ready" state.
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
      odysseySdFormatting() || otaBusy() || sleepPending) return;
  if (!odysseySdReady()) {
    Serial.println("[TOUCH] SD unavailable; recording task will recover before capture");
  }
  odysseyStopRequested=false;
  odysseyRecordingStartedAt=millis();
  odysseyRecordFaultAt=0;
  odysseyRecorderFaultLedCode=0;
  odysseyRecording=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseyStopRequested=false;
    odysseyRecordFaultAt=millis();
    odysseyRecorderFaultLedCode=4;
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> SD audio START");
}
#endif
