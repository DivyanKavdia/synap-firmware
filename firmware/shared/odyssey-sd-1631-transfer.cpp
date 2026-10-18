// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// C3 storage is mounted through the stock Arduino SD SPI path and accessed through FAT/VFS.
// Files are deleted only after the PWA has imported and verified them.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0,sessionToken=0;
  uint8_t operation=0,credits=6,paceMs=15;
  bool compact=false;
  char path[64]{};
};
// Shipping safety policy: physical C3+SD units have timed out on CMD24/CMD25
// and become unavailable after FAT unlink/format. Until a verified hardware
// and SDSPI fix exists, firmware itself must reject ALL BLE filesystem
// mutators, including commands issued by cached/older PWA clients.
// Does not gate disconnected recording, catalogue or verified SD reads.
static constexpr bool ODYSSEY_C3_SD_BLE_SINGLE_DELETE_ENABLED=true;
static constexpr bool ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED=false;
// One-file Delete is opt-in and guarded; bulk Clear stays disabled.
// Format has its own explicit request token.
static constexpr bool ODYSSEY_C3_SD_EXPLICIT_FORMAT_ENABLED=true;
static constexpr uint32_t ODYSSEY_C3_SD_FORMAT_TOKEN=0x5344464Du; // "SDFM"
static constexpr uint32_t ODYSSEY_C3_SD_RESCUE_TOKEN=0x53445246u; // "SDRF"
static QueueHandle_t requests=nullptr;
// Keep SD reads protected while BLE is transferring and briefly afterward.
static std::atomic<bool> sdBleTransferInFlight{false};
static std::atomic<uint32_t> sdBleLastTransferAt{0};
bool busy() {
  if (sdBleTransferInFlight.load()) return true;
  const uint32_t last=sdBleLastTransferAt.load();
  return last && uint32_t(millis()-last)<15000u;
}
// Legacy opcode 12 uses six paced notifications. Opcode 30 accepts receiver
// credits, capped at 12 on C3/Bluefy to prevent NimBLE queue overrun when iOS
// reports MTU 517 but delivers only a fraction of 20 notifications.
// Opcode 31 is opt-in: same contiguous payload/credit contract with receiver-
// selected 8..15 ms pacing. Legacy 12/30 keep their original timing and ABI.

static BLECharacteristic* streamCharacteristic=nullptr;
static constexpr uint32_t TRANSFER_STACK_BYTES=8192;
static portMUX_TYPE responseMux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t response[496]{};
static size_t responseSize=16;
static uint32_t responseConnection=0;
static char selectedPath[64]{};
// Only a successfully selected WAV on this connection can authorize an
// opcode31 compact window. No filesystem handle is retained between windows.
static uint32_t selectedSessionToken=0,selectedSessionConnection=0;
// A successful op3 stat locks the read-only file size to this selection and
// connection. Reuse it across 4 KiB cache fills rather than stat() the same
// FAT entry hundreds of times. No file handles survive BLE waits.
static uint32_t selectedFileSize=0;
static String catalogueBuffer;
static int catalogueErrno=0;
static uint8_t catalogueStep=0;
// Live media I/O failure is independent of the persisted offline recorder fault.
// Never present an old stage-66 WAV error as a new sync/delete failure.
static std::atomic<uint8_t> sdLastMediaFaultOperation{0};
static std::atomic<uint8_t> sdLastMediaFaultStep{0};
static std::atomic<int32_t> sdLastMediaFaultErrno{0};
static void noteSdMediaFault(uint8_t operation,uint8_t step,int error) {
  sdLastMediaFaultOperation=operation;
  sdLastMediaFaultStep=step;
  sdLastMediaFaultErrno=error?error:EIO;
  Serial.printf("[SD] media fault op=%u step=%u errno=%d (existing WAVs retained)\n",
    unsigned(operation),unsigned(step),int(sdLastMediaFaultErrno.load()));
}

enum : uint8_t {
  OK=0,BUSY=1,BAD_COMMAND=2,NO_SD=3,IO_ERROR=7,FILE_UNAVAILABLE=11
};

static bool safeWavPath(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12 || n>63 ||
      (strcmp(path+n-4,".wav")!=0 && strcmp(path+n-4,".WAV")!=0)) return false;
  for (size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}
static bool storageReady() {
  // Normal PWA reads are observational only. Only operation 14 may remount.
  return odysseySdReady();
}
static bool fullPath(const char* logical,char* full,size_t capacity) {
  return safeWavPath(logical) && odysseySdPath(logical,full,capacity);
}

static void virtualWavHeader(uint8_t* h,uint32_t totalBytes) {
  memset(h,0,44);
  const uint32_t pcmBytes=totalBytes>44u?totalBytes-44u:0u;
  memcpy(h,"RIFF",4);put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16u);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,pcmBytes);
}
static void patchVirtualWavHeader(uint8_t* bytes,size_t size,uint32_t offset,uint32_t totalBytes) {
  if (!bytes || !size || totalBytes<44u || offset>=44u) return;
  uint8_t header[44];
  virtualWavHeader(header,totalBytes);
  const size_t count=std::min(size_t(44u-offset),size);
  memcpy(bytes,header+offset,count);
}

static void reply(const Request& request,uint8_t error,uint32_t total=0,uint32_t offset=0,
                  const uint8_t* bytes=nullptr,size_t size=0) {
  uint8_t value[496]{};
  value[0]=0xCB;value[1]=1;value[2]=error?2:1;value[3]=error;
  put32le(value+4,request.id);put32le(value+8,total);put32le(value+12,offset);
  size=std::min(size,size_t(480));
  if (size && bytes) memcpy(value+16,bytes,size);
  portENTER_CRITICAL(&responseMux);
  if (request.connection==connectionGeneration.load()) {
    memcpy(response,value,16+size);
    responseSize=16+size;
    responseConnection=request.connection;
  }
  portEXIT_CRITICAL(&responseMux);
}

static uint8_t selectFile(const char* path,uint32_t& total) {
  selectedPath[0]=0;
  selectedFileSize=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0) return FILE_UNAVAILABLE;
  if (uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  selectedFileSize=total;
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

// BLE SD downloads are sequential, but the transport has a 480-byte reply
// ceiling. The old path fopen/fseek/fread/fclose'd for EVERY 480-byte request.
// A 1 MiB WAV triggered >2,000 FAT directory traversals. Cache one already
// CLOSED 4 KiB read; no file handle or mutex survives across BLE waits.
// This is READ-ONLY, never used by local offline recording.
static constexpr uint32_t ODYSSEY_SD_BLE_READ_CACHE_BYTES=4096u;
struct OdysseySdBleReadCache {
  bool valid=false;
  uint32_t connection=0,total=0,start=0,used=0;
  char path[64]{};
  alignas(4) uint8_t bytes[ODYSSEY_SD_BLE_READ_CACHE_BYTES]{};
};
static OdysseySdBleReadCache sdBleReadCache;
static uint8_t sdBleReadFailureStep=0; // 1=stat,2=open,3=seek,4=read,5=close
static int sdBleReadFailureErrno=0;
static uint32_t sdBleReadCacheHits=0,sdBleReadDiskFills=0;
// Counters are media-worker-owned, RAM-only and updated by existing reads.
// A diagnostic request NEVER probes, reads or modifies the physical SD card.
static uint32_t sdBleReadRequests=0,sdBleReadLastRequestedOffset=0;
static uint32_t sdBleReadLastOkOffset=0,sdBleReadFailureOffset=0;
static uint32_t sdBleReadWindowRequests=0,sdBleReadMtuFallbacks=0;
// Last failed cache-fill snapshot, retained in RAM even when VFS quarantines
// the card. Flags: bit0=ferror, bit1=feof, bit2=seek fail, bit3=close fail.
static uint32_t sdBleFaultReadGot=0,sdBleFaultReadWant=0,sdBleFaultReadFlags=0;
static void invalidateBleReadCache() {
  sdBleReadCache.valid=false;
  sdBleReadCache.path[0]=0;
}
static uint8_t readWavChunk(const char* logical,uint32_t offset,
    uint32_t& total,uint8_t* bytes,size_t& size) {
  sdBleReadFailureStep=0;
  sdBleReadFailureErrno=0;
  ++sdBleReadRequests;
  sdBleReadLastRequestedOffset=offset;
  if (!safeWavPath(logical) || !bytes) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) {
    invalidateBleReadCache();
    return NO_SD;
  }
  char full[96];
  if (!fullPath(logical,full,sizeof(full))) return BAD_COMMAND;
  const uint32_t generation=connectionGeneration.load();
  const auto& cache=sdBleReadCache;
  if (cache.valid && cache.connection==generation &&
      !strcmp(cache.path,logical) && offset>=cache.start &&
      uint64_t(offset)<uint64_t(cache.start)+cache.used) {
    total=cache.total;
    const uint32_t available=cache.used-(offset-cache.start);
    size=std::min(size_t(480),size_t(available));
    memcpy(bytes,cache.bytes+(offset-cache.start),size);
    patchVirtualWavHeader(bytes,size,offset,total);
    ++sdBleReadCacheHits;
    sdBleReadLastOkOffset=offset+uint32_t(size);
    return OK;
  }
  invalidateBleReadCache();
  // Op3 already verified the file is regular and its length on this very
  // connection. No SD mutations are allowed over BLE on deployed C3+SD.
  // The streaming/recording gate prevents simultaneous writers. If the
  // selection changes, use the original stat() path without shortcuts.
  const bool selectedMetadataValid=selectedSessionToken!=0 &&
    selectedSessionConnection==generation && selectedFileSize>0 &&
    selectedPath[0] && strcmp(logical,selectedPath)==0;
  if (selectedMetadataValid) {
    total=selectedFileSize;
  } else {
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) {
      const int err=errno;
      if (err==EIO || err==ENODEV || err==ETIMEDOUT) {
        sdBleReadFailureStep=1;
        sdBleReadFailureErrno=err;
        errno=err;
        return IO_ERROR;
      }
      return FILE_UNAVAILABLE;
    }
    if (!S_ISREG(st.st_mode) || st.st_size<=0 ||
        uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
    total=uint32_t(st.st_size);
  }
  if (offset>=total) return FILE_UNAVAILABLE;
  const uint32_t base=(offset/ODYSSEY_SD_BLE_READ_CACHE_BYTES)*
    ODYSSEY_SD_BLE_READ_CACHE_BYTES;
  // C3's long file offsets are signed; the app limits WAV downloads to
  // 64 MiB, but reject impossible offsets instead of seeking backwards.
  if (base>2147483647u) return FILE_UNAVAILABLE;
  const size_t want=std::min(size_t(ODYSSEY_SD_BLE_READ_CACHE_BYTES),
    size_t(total-base));
  errno=0;
  FILE* file=fopen(full,"rb");
  if (!file) {
    const int err=errno?errno:EIO;
    if (err==ENOENT) return FILE_UNAVAILABLE;
    sdBleReadFailureStep=2;sdBleReadFailureErrno=err;errno=err;
    return IO_ERROR;
  }
  errno=0;
  const bool seekOk=fseek(file,long(base),SEEK_SET)==0;
  const int seekErr=errno;
  size_t got=0;
  if (seekOk) {
    // Avoid requesting large multi-sector CMD18 bursts on hardware whose
    // controller previously became stuck busy after SD operations. Fill one
    // 4 KiB cache with at most eight 512-byte fread calls, keeping the WAV
    // open only during this bounded synchronous loop.
    while (got<want) {
      const size_t step=std::min(size_t(512),want-got);
      errno=0;
      const size_t amount=fread(sdBleReadCache.bytes+got,1,step,file);
      got+=amount;
      if (amount!=step) break;
    }
  }
  const int readErr=errno;
  const bool streamError=ferror(file)!=0;
  const bool streamEof=feof(file)!=0;
  errno=0;
  const int closeResult=fclose(file);
  const int closeErr=errno;
  if (!seekOk || got!=want || closeResult!=0) {
    sdBleFaultReadGot=uint32_t(got);
    sdBleFaultReadWant=uint32_t(want);
    sdBleFaultReadFlags=(streamError?1u:0u) | (streamEof?2u:0u) |
      (!seekOk?4u:0u) | (closeResult!=0?8u:0u);
    const int err=!seekOk ? (seekErr?seekErr:EIO) :
      got!=want ? (readErr?readErr:EIO) :
      (closeErr?closeErr:EIO);
    sdBleReadFailureStep=!seekOk?3:got!=want?4:5;
    sdBleReadFailureErrno=err;
    errno=err;
    invalidateBleReadCache();
    return IO_ERROR;
  }
  // Cache is published only after fclose succeeds and the entire bounded
  // window has been read. Read errors never expose partial SD bytes.
  sdBleReadCache.connection=generation;
  sdBleReadCache.total=total;
  sdBleReadCache.start=base;
  sdBleReadCache.used=uint32_t(want);
  snprintf(sdBleReadCache.path,sizeof(sdBleReadCache.path),"%s",logical);
  sdBleReadCache.valid=true;
  ++sdBleReadDiskFills;
  const uint32_t available=uint32_t(want)-(offset-base);
  size=std::min(size_t(480),size_t(available));
  memcpy(bytes,sdBleReadCache.bytes+(offset-base),size);
  patchVirtualWavHeader(bytes,size,offset,total);
  sdBleReadLastOkOffset=offset+uint32_t(size);
  return OK;
}

static uint8_t readSelected(const char* requestedPath,uint32_t offset,
    uint32_t& total,uint8_t* bytes,size_t& size) {
  // Explicit path never depends on a previous request's selected file.
  if (requestedPath && requestedPath[0]) {
    if (!strcmp(requestedPath,"@catalogue")) {
      total=catalogueBuffer.length();
      if (!total || offset>=total) return FILE_UNAVAILABLE;
      size=std::min(size_t(480),size_t(total-offset));
      memcpy(bytes,catalogueBuffer.c_str()+offset,size);
      return OK;
    }
    if (!safeWavPath(requestedPath)) return BAD_COMMAND;
    if (offset==0) Serial.printf("[SD] transfer begin path=%s\\n",requestedPath);
    return readWavChunk(requestedPath,offset,total,bytes,size);
  }
  // Preserve legacy selected-file or RAM-backed catalogue semantics.
  if (!selectedPath[0]) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    memcpy(bytes,catalogueBuffer.c_str()+offset,size);
    return OK;
  }
  return readWavChunk(selectedPath,offset,total,bytes,size);
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  catalogueErrno=0;catalogueStep=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  if (!catalogueBuffer.reserve(2048)) {
    catalogueErrno=ENOMEM;catalogueStep=2;return IO_ERROR;
  }
  catalogueBuffer="[";
  unsigned count=0;
  // Index the FAT root's compact Rxxxxxxx.WAV files first, then legacy
  // /synap recordings. Both retain their existing logical /synap/... paths.
  // Missing legacy directory is normal on a new/empty card. No mkdir/write.
  const char* const dirs[2]={odysseySdMountPoint(),ODYSSEY_SD_RECORDING_DIR};
  for (uint8_t scan=0;scan<2 && !catalogueErrno;++scan) {
    errno=0;
    DIR* directory=opendir(dirs[scan]);
    if (!directory) {
      const int directoryErrno=errno;
      if (scan==1u && directoryErrno==ENOENT) continue;
      catalogueErrno=directoryErrno?directoryErrno:EIO;
      catalogueStep=1;break;
    }
    for (;;) {
      errno=0;
      dirent* entry=readdir(directory);
      if (!entry) {
        if (errno) {catalogueErrno=errno;catalogueStep=3;}
        break;
      }
      const char* name=entry->d_name;
      if (!name || !strcmp(name,".") || !strcmp(name,"..")) continue;
      // Disjoint file namespaces prevent two filesystem entries from
      // resolving to the same media-v1 logical file identifier.
      if (scan==0u ? !odysseySdRootWavName(name)
                    : odysseySdRootWavName(name)) continue;
      char logical[64]{};
      const int n=snprintf(logical,sizeof(logical),"/synap/%s",name);
      if (n<=0 || size_t(n)>=sizeof(logical) ||
          !safeWavPath(logical)) continue;
      char full[96]{};
      if (!odysseySdPath(logical,full,sizeof(full))) continue;
      struct stat st{};
      errno=0;
      if (stat(full,&st)!=0) {
        catalogueErrno=errno?errno:EIO;catalogueStep=4;break;
      }
      if (!S_ISREG(st.st_mode)) continue;
      if (st.st_size<0 || uint64_t(st.st_size)>UINT32_MAX) {
        catalogueErrno=EOVERFLOW;catalogueStep=5;break;
      }
      if (count>=100u) {catalogueErrno=EOVERFLOW;catalogueStep=6;break;}
      if (count++) catalogueBuffer+=",";
      catalogueBuffer+="{\"path\":\""+String(logical)+
        "\",\"bytes\":"+String(uint32_t(st.st_size))+"}";
    }
    if (closedir(directory)!=0 && !catalogueErrno) {
      catalogueErrno=errno?errno:EIO;catalogueStep=7;
    }
  }
  if (catalogueErrno) {
    catalogueBuffer="";
    Serial.printf("[SD] catalogue failed step=%u errno=%d; existing WAVs retained\\n",
      unsigned(catalogueStep),catalogueErrno);
    return IO_ERROR; // Never publish a partial list after a read fault.
  }
  catalogueBuffer+="]";
  total=catalogueBuffer.length();
  return OK;
}

static uint8_t removeFailureStep=0;
static uint8_t removeFile(const char* path) {
  removeFailureStep=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  // User controls the hardware power switch; battery ADC is telemetry only.
  // No arbitrary 3.8V/3.9V admission rule for selected-file SD deletion.
  // Active recording, timeout quarantine, verified FAT mount and storage
  // mutex still enforce safe ordering. Physical brownout remains possible.
  OdysseySdGuard guard(pdMS_TO_TICKS(1000));
  if (!guard || !storageReady() ||
      odysseySdWriteTimeoutQuarantined() ||
      odysseySdReadTimeoutQuarantined()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  errno=0;
  if (stat(full,&st)!=0) {
    if (errno==ENOENT) return OK;
    removeFailureStep=1;return IO_ERROR; // stat before FAT mutation
  }
  if (!S_ISREG(st.st_mode)) return FILE_UNAVAILABLE;
  errno=0;
  if (unlink(full)!=0) {
    if (errno==ENOENT) return OK; // idempotent exact-path delete
    removeFailureStep=2;
    return IO_ERROR; // failed FAT write: no retry, read, or remount
  }
  // Do not issue an immediate post-unlink FAT stat: the card may still be
  // programming. PWA verifies deletion after a settle interval with a new
  // catalogue, and preserves the sync receipt if verification fails.
  selectedPath[0]=0;
  selectedSessionToken=0;
  selectedFileSize=0;
  catalogueBuffer="";
  return OK;
}

static uint16_t clearRecordings() {
  // Do not gate explicit SD operations on estimated battery voltage.
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return 0;
  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) return 0;
  DIR* directory=opendir(directoryPath);
  if (!directory) return 0;

  String logicalPaths[100];
  uint16_t count=0;
  while (dirent* entry=readdir(directory)) {
    if (count>=100) break;
    if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    char logical[64];
    const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
    char full[96];
    if (!odysseySdPath(logical,full,sizeof(full))) continue;
    struct stat st{};
    if (stat(full,&st)==0 && S_ISREG(st.st_mode)) logicalPaths[count++]=logical;
  }
  closedir(directory);

  uint16_t removed=0;
  for (uint16_t i=0;i<count;++i) {
    char full[96];
    if (odysseySdPath(logicalPaths[i].c_str(),full,sizeof(full)) && unlink(full)==0) ++removed;
  }
  return removed;
}

// Fast media-v1 window, using the same per-chunk, explicit-path FAT read as
// fallback op4. A bounded window never owns the SD mutex across BLE waits.
static void notifySdWindow(const Request& request,uint8_t kind,uint8_t error,
    uint32_t total,uint32_t offset,const uint8_t* payload=nullptr,size_t length=0) {
  if (!streamCharacteristic || request.connection!=connectionGeneration.load() ||
      !deviceConnected.load()) return;
  uint8_t packet[496]{};
  packet[0]=0xCC;packet[1]=1;packet[2]=kind;packet[3]=error;
  put32le(packet+4,request.id);put32le(packet+8,total);put32le(packet+12,offset);
  length=std::min(length,size_t(480));
  if (payload && length) memcpy(packet+16,payload,length);
  streamCharacteristic->setValue(packet,16+length);
  streamCharacteristic->notify();
}

static void streamSdWindow(const Request& request) {
  ++sdBleReadWindowRequests;
  // A short 16-byte media-v3 request carries the selection request ID,
  // not a path. Match it to the SAME BLE connection and original selectFile.
  // Old explicit-path opcode30/31 callers bypass this v3-only optimization.
  const char* windowPath=request.path;
  if (request.compact) {
    if (!request.sessionToken || request.sessionToken!=selectedSessionToken ||
        request.connection!=selectedSessionConnection || !selectedPath[0]) {
      notifySdWindow(request,2,FILE_UNAVAILABLE,0,request.offset);
      return;
    }
    windowPath=selectedPath;
  }
  // Older/small-MTU Bluefy connections fall back to op4 without overlong BLE
  // notifications. A 16-byte empty-window completion tells the PWA to fall back.
  const uint16_t capacity=attValueCapacity.load();
  if (capacity<496u || !streamCharacteristic || !windowPath[0]) {
    ++sdBleReadMtuFallbacks;
    notifySdWindow(request,2,0,0,request.offset);
    return;
  }
  const size_t chunkCapacity=std::min(size_t(480),size_t(capacity-16u));
  uint8_t chunk[480]{};
  uint32_t offset=request.offset,fullSize=0;
  uint8_t error=OK;
  // Fast op30 uses only a bounded BLE window. 20 x 480-byte at 8 ms proved
  // lossy on physical Bluefy (6/20 packets delivered) during SD recovery.
  // Prioritize contiguous verified bytes over fire-and-forget throughput.
  // Legacy op12 remains six packets at 20 ms, completely unchanged.
  const bool adaptive=request.operation==31u;
  const bool fast=request.operation==30u || adaptive;
  const uint8_t credits=fast
    ?std::max(uint8_t(6),std::min(uint8_t(12),request.credits))
    :uint8_t(6);
  // The phone owns rate selection. A lossy burst must retry from the first
  // missing offset instead of falsely treating a queued notify as delivered.
  const uint8_t paceMs=adaptive?std::max(uint8_t(8),std::min(uint8_t(15),request.paceMs))
    :uint8_t(fast?15u:20u);
  const TickType_t pacing=pdMS_TO_TICKS(paceMs);
  for (uint8_t n=0;n<credits;++n) {
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load() ||
        streamingEnabled.load() || odysseyRecording.load() || OdysseyWifi::busy() ||
        otaBusy() || sleepPending) return;
    uint32_t total=0;
    size_t length=0;
    error=readSelected(windowPath,offset,total,chunk,length);
    if (error!=OK || !length || length>chunkCapacity) {
      if (error==IO_ERROR) {
        sdBleReadFailureOffset=sdBleReadLastRequestedOffset;
        noteSdMediaFault(12,sdBleReadFailureStep,sdBleReadFailureErrno);
        errno=sdBleReadFailureErrno?sdBleReadFailureErrno:EIO;
        odysseySdMarkVfsFailure();
        invalidateBleReadCache();
      }
      if (error==OK) error=BAD_COMMAND;
      notifySdWindow(request,2,error,fullSize,offset);
      return;
    }
    if (fullSize && total!=fullSize) {
      notifySdWindow(request,2,FILE_UNAVAILABLE,fullSize,offset);
      return;
    }
    fullSize=total;
    // An ATT notification is not an ACK. The receiver checks offsets, can
    // resume missing windows, and verifies the final file before import.
    notifySdWindow(request,1,OK,fullSize,offset,chunk,length);
    offset+=uint32_t(length);
    // Keep END even for a full credit grant: the phone can finish early
    // after contiguous delivery, but if one packet is lost the END lets it
    // retry immediately instead of waiting for a 3-second missing-END timer.
    if (offset>=fullSize) break;
    vTaskDelay(pacing);
  }
  // End packet closes the credit window. Never delete/modify the SD source.
  if (request.connection==connectionGeneration.load() && deviceConnected.load()) {
    vTaskDelay(pacing);
    notifySdWindow(request,2,OK,fullSize,offset);
  }
}

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
    if (xQueueReceive(requests,&request,pdMS_TO_TICKS(500))!=pdTRUE) {
      // Never remount merely because the worker is idle or catalogue failed.
      // A physical disconnected double-tap is an explicit recovery request,
      // just like PWA operation 14, and may safely run while storage is idle.
      const bool idleEnough=!odysseyRecording.load() && !streamingEnabled.load() &&
        !OdysseyWifi::busy() && !otaBusy() && !sleepPending;
      if (odysseySdConsumeRecoveryRequest()) {
        if (idleEnough) {
          Serial.println("[SD] physical touch requested software recovery");
          (void)odysseyRecoverSdCard("touch");
        } else {
          odysseySdRequestRecovery();
        }
      }
      continue;
    }
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load()) continue;
    // Gate before touching FAT, the read cache, catalogue, or any remount.
    // BAD_COMMAND is the existing media-v1 'unsupported' error: no new ABI.
    if ((!ODYSSEY_C3_SD_BLE_SINGLE_DELETE_ENABLED && request.operation==17u) ||
        (!ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED && request.operation==18u) ||
        (request.operation==19u &&
         (!ODYSSEY_C3_SD_EXPLICIT_FORMAT_ENABLED ||
          request.offset!=ODYSSEY_C3_SD_FORMAT_TOKEN ||
          request.path[0])) ||
        (request.operation==33u &&
         (request.offset!=ODYSSEY_C3_SD_RESCUE_TOKEN || request.path[0]))) {
      Serial.printf("[SD] BLE maintenance write refused op=%u; originals preserved\\n",
        unsigned(request.operation));
      reply(request,BAD_COMMAND);
      continue;
    }
    sdBleTransferInFlight=true;
    sdBleLastTransferAt=millis();
    // Connected remote standby only idles the microphone/CPU; SD media must
    // remain readable for verified sync and recovery without a forced wake.
    if (odysseyRecording.load() || streamingEnabled.load() || OdysseyWifi::busy() ||
        otaBusy() || sleepPending) {
      reply(request,BUSY);
      sdBleLastTransferAt=millis();
      sdBleTransferInFlight=false;
      continue;
    }
    uint8_t error=OK;uint32_t total=0;size_t size=0;
    switch (request.operation) {
      case 3:
        error=selectFile(request.path,total);
        selectedSessionToken=error==OK?request.id:0;
        selectedSessionConnection=request.connection;
        break;
      case 12:
      case 30:
      case 31: // Adaptive paced BLE read; all three remain read-only.
        streamSdWindow(request);
        sdBleLastTransferAt=millis();
        sdBleTransferInFlight=false;
        continue;
      case 16: error=OK; break;
      case 27: {
        // Purely read-only snapshot of the LAST recorder error after a
        // reboot. It never calls SD.begin, SD.end, mkdir, fopen or recovery.
        // Clients request this only when capabilities report stage >=40.
        char detail[480];
        // wrD is a software 0x20SS00EE VFS fault marker, not an observed
        // raw SD command. The failing offset helps isolate FAT cluster-boundary
        // allocation from electrical/busy-wire faults without reading SD.
        const uint8_t stage=odysseyLastRecordFailureStage();
        const uint32_t failureOffset=stage>=66u && stage<=70u
          ? odysseyLastRecordFailureBytes()+44u : 0u;
        int n=snprintf(detail,sizeof(detail),
          "{\"version\":1,\"sdState\":%u,\"sdLiveProbe\":%u,\"recordStage\":%u,\"recordBytes\":%lu,\"wrE\":%lu,\"wrD\":%lu,\"wrN\":%lu,\"wrX\":%lu,\"wrF\":%lu,\"vfsStep\":%u,\"vfsErrno\":%ld,\"failureOffset\":%lu,\"sdClockKhz\":%lu,\"faultKind\":\"vfs\",\"ioOp\":%u,\"ioStep\":%u,\"ioErr\":%ld,\"wireCmd\":%lu,\"wireErr\":%lu,\"wireArg\":%lu,\"wireRsp\":%lu,\"wireR2\":%lu,\"wireMs\":%lu,\"wireBudget\":%lu,\"wireLen\":%lu,\"wirePrev\":%lu,\"wireWriteOk\":%lu}",
          unsigned(odysseySdDetectionState()),
          unsigned(odysseySdProbeState()),
          unsigned(odysseyLastRecordFailureStage()),
          static_cast<unsigned long>(odysseyLastRecordFailureBytes()),
          static_cast<unsigned long>(odysseyLastWriteErrno()),
          static_cast<unsigned long>(odysseyLastDriverWriteFault()),
          static_cast<unsigned long>(odysseyLastWriteReturned()),
          static_cast<unsigned long>(odysseyLastWriteExpected()),
          static_cast<unsigned long>(odysseyLastWriteFerror()),
          unsigned(odysseySdVfsStepValue()),
          static_cast<long>(odysseySdVfsErrnoValue()),
          static_cast<unsigned long>(failureOffset),
          static_cast<unsigned long>(odysseyLastWireClockKhz()),
          unsigned(sdLastMediaFaultOperation.load()),
          unsigned(sdLastMediaFaultStep.load()),
          static_cast<long>(sdLastMediaFaultErrno.load()),
          static_cast<unsigned long>(odysseyLastWireCommand()),
          static_cast<unsigned long>(odysseyLastWireError()),
          static_cast<unsigned long>(odysseyLastWireArgument()),
          static_cast<unsigned long>(odysseyLastWireResponse()),
          static_cast<unsigned long>(odysseyLastWireCmd13()),
          static_cast<unsigned long>(odysseyLastWireMs()),
          static_cast<unsigned long>(odysseyLastWireBudget()),
          static_cast<unsigned long>(odysseyLastWireLen()),
          static_cast<unsigned long>(odysseyLastWirePrev()),
          static_cast<unsigned long>(odysseyLastWireGoodWrites()));
        // Always return meaningful native fault context, even if future
        // counters reach their maximum uint32 decimal width. BLE op27 must
        // never truncate a JSON payload or silently lose the recorder error.
        if (n>=int(sizeof(detail))) {
          n=snprintf(detail,sizeof(detail),
            "{\"version\":1,\"recordStage\":%u,\"recordBytes\":%lu,\"wireCmd\":%lu,\"wireErr\":%lu,\"wireArg\":%lu,\"wireMs\":%lu,\"wireBudget\":%lu,\"wireLen\":%lu,\"wirePrev\":%lu,\"wireWriteOk\":%lu}",
            unsigned(stage),
            static_cast<unsigned long>(odysseyLastRecordFailureBytes()),
            static_cast<unsigned long>(odysseyLastWireCommand()),
            static_cast<unsigned long>(odysseyLastWireError()),
            static_cast<unsigned long>(odysseyLastWireArgument()),
            static_cast<unsigned long>(odysseyLastWireMs()),
            static_cast<unsigned long>(odysseyLastWireBudget()),
            static_cast<unsigned long>(odysseyLastWireLen()),
            static_cast<unsigned long>(odysseyLastWirePrev()),
            static_cast<unsigned long>(odysseyLastWireGoodWrites()));
        }
        if (n<0 || size_t(n)>=sizeof(detail)) { error=IO_ERROR;break; }
        reply(request,OK,uint32_t(n),0,reinterpret_cast<const uint8_t*>(detail),size_t(n));
        sdBleLastTransferAt=millis();
        sdBleTransferInFlight=false;
        continue;
      }
      case 28: {
        // Explicit sector0 + bounded FAT VBR inspection, no writes/mounts.
        odysseySdRawProbeOnce();
        const OdysseySdRawProbeResult raw=odysseySdRawProbeCached;
        char detail[480];
        const int n=snprintf(detail,sizeof(detail),
          "{\"version\":2,\"status\":%u,\"error\":%ld,\"sectors\":%lu,\"sectorBytes\":%u,\"signature\":%u,\"fatHint\":%u,\"partitionLba\":%lu,\"partitionType\":%u,\"partitionIndex\":%u,\"partitionSectors\":%lu,\"partitionBounds\":%u,\"vbrRead\":%u,\"vbrError\":%ld,\"vbrSignature\":%u,\"bpbPlausible\":%u,\"fatVariant\":%u,\"bpbBytesPerSector\":%u,\"bpbSectorsPerCluster\":%u,\"bpbTotalSectors\":%lu,\"firstMountErr\":%ld}",
          unsigned(raw.status),static_cast<long>(raw.error),
          static_cast<unsigned long>(raw.sectors),unsigned(raw.sectorBytes),
          unsigned(raw.signature),unsigned(raw.fatHint),
          static_cast<unsigned long>(raw.partitionLba),
          unsigned(raw.partitionType),unsigned(raw.partitionIndex),
          static_cast<unsigned long>(raw.partitionSectors),
          unsigned(raw.partitionBounds),unsigned(raw.vbrRead),
          static_cast<long>(raw.vbrError),unsigned(raw.vbrSignature),
          unsigned(raw.bpbPlausible),unsigned(raw.fatVariant),
          unsigned(raw.bpbBytesPerSector),unsigned(raw.bpbSectorsPerCluster),
          static_cast<unsigned long>(raw.bpbTotalSectors),
          static_cast<long>(odysseySdFirstMountFailureError()));
        if (n<=0 || size_t(n)>=sizeof(detail)) { error=IO_ERROR;break; }
        reply(request,OK,uint32_t(n),0,reinterpret_cast<const uint8_t*>(detail),size_t(n));
        sdBleLastTransferAt=millis();sdBleTransferInFlight=false;
        continue;
      }
      case 34: {
        // SD-independent recorder status, safe even after native CMD25/EIO.
        // Zero reads, zero FAT writes and no mount/recovery side effects.
        char detail[280];
        const uint8_t stage=odysseyLastRecordFailureStage();
        const int n=snprintf(detail,sizeof(detail),
          "{\"version\":1,\"sdState\":%u,\"sdLiveProbe\":%u,\"stage\":%u,\"bytes\":%lu,\"faultLed\":%u,\"micRestarts\":%u,\"recording\":%u,\"stopPending\":%u,\"readQuarantine\":%u,\"writeQuarantine\":%u,\"coldRecovery\":%u,\"resetReason\":%u}",
          unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
          unsigned(stage),static_cast<unsigned long>(odysseyLastRecordFailureBytes()),
          unsigned(odysseyRecorderFaultLedCode.load()),
          unsigned(odysseyMicRecoveryAttempts.load()),
          unsigned(odysseyRecording.load()?1u:0u),
          unsigned(odysseyStopRequested.load()?1u:0u),
          unsigned(odysseySdReadTimeoutQuarantined()?1u:0u),
          unsigned(odysseySdWriteTimeoutQuarantined()?1u:0u),
          unsigned(odysseySdColdRecoveryStatus()),
          unsigned(bootResetReason));
        if (n<=0 || size_t(n)>=sizeof(detail)) { error=IO_ERROR;break; }
        reply(request,OK,uint32_t(n),0,reinterpret_cast<const uint8_t*>(detail),size_t(n));
        sdBleLastTransferAt=millis();sdBleTransferInFlight=false;
        continue;
      }
      case 29: {
        // SD-independent sync trace. This reads only RAM counters from
        // previous op4/op12 requests; it must remain safe after mount loss.
        char detail[480];
        const int n=snprintf(detail,sizeof(detail),
          "{\"version\":1,\"sdState\":%u,\"sdLiveProbe\":%u,\"ioOp\":%u,\"ioStep\":%u,\"ioErr\":%ld,\"reads\":%lu,\"cacheHits\":%lu,\"diskFills\":%lu,\"lastRequestedOffset\":%lu,\"lastOkOffset\":%lu,\"faultOffset\":%lu,\"windowRequests\":%lu,\"mtuFallbacks\":%lu,\"wireCmd\":%lu,\"wireErr\":%lu,\"wireArg\":%lu,\"wireMs\":%lu,\"readGot\":%lu,\"readWant\":%lu,\"readFlags\":%lu}",
          unsigned(odysseySdDetectionState()),
          unsigned(odysseySdProbeState()),
          unsigned(sdLastMediaFaultOperation.load()),
          unsigned(sdLastMediaFaultStep.load()),
          static_cast<long>(sdLastMediaFaultErrno.load()),
          static_cast<unsigned long>(sdBleReadRequests),
          static_cast<unsigned long>(sdBleReadCacheHits),
          static_cast<unsigned long>(sdBleReadDiskFills),
          static_cast<unsigned long>(sdBleReadLastRequestedOffset),
          static_cast<unsigned long>(sdBleReadLastOkOffset),
          static_cast<unsigned long>(sdBleReadFailureOffset),
          static_cast<unsigned long>(sdBleReadWindowRequests),
          static_cast<unsigned long>(sdBleReadMtuFallbacks),
          static_cast<unsigned long>(odysseySdWireFaultCmd()),
          static_cast<unsigned long>(odysseySdWireFaultError()),
          static_cast<unsigned long>(odysseySdWireFaultArg()),
          static_cast<unsigned long>(odysseySdWireFaultElapsedMs()),
          static_cast<unsigned long>(sdBleFaultReadGot),
          static_cast<unsigned long>(sdBleFaultReadWant),
          // Bits 0..3 retain their original read-failure semantics.
          // Bit4=timed-out CMD17/18 read, bit5=timed-out CMD24/25 write.
          // No additional JSON keys: op29 must fit its 480-byte BLE reply.
          static_cast<unsigned long>(sdBleFaultReadFlags |
            (odysseySdReadTimeoutQuarantined()?16u:0u) |
            (odysseySdWriteTimeoutQuarantined()?32u:0u)));
        if (n<0 || size_t(n)>=sizeof(detail)) { error=IO_ERROR;break; }
        reply(request,OK,uint32_t(n),0,reinterpret_cast<const uint8_t*>(detail),size_t(n));
        sdBleLastTransferAt=millis();sdBleTransferInFlight=false;
        continue;
      }
      case 4:
        error=readSelected(request.path,request.offset,total,bytes,size);
        if (error==IO_ERROR) {
          sdBleReadFailureOffset=sdBleReadLastRequestedOffset;
          noteSdMediaFault(4,sdBleReadFailureStep,sdBleReadFailureErrno);
          errno=sdBleReadFailureErrno?sdBleReadFailureErrno:EIO;
          odysseySdMarkVfsFailure();
          invalidateBleReadCache();
        }
        break;
      case 7:
        error=catalogue(total);
        // After an SD metadata mutation, a card may still be completing
        // internal programming. Retry ONE observational catalogue read after
        // a bounded settle interval before declaring storage unavailable.
        // No write/delete/reinit/remount is repeated. A partial catalogue
        // remains an error, never a verified file absence.
        if (error==IO_ERROR &&
            (catalogueErrno==EIO || catalogueErrno==ENODEV ||
             catalogueErrno==ETIMEDOUT) &&
            request.connection==connectionGeneration.load() &&
            deviceConnected.load() && !odysseyRecording.load()) {
          const int firstError=catalogueErrno;
          const uint8_t firstStep=catalogueStep;
          Serial.printf("[SD] catalogue transient I/O op7 step=%u errno=%d; one read-only retry\\n",
            unsigned(firstStep),firstError);
          vTaskDelay(pdMS_TO_TICKS(120u));
          if (request.connection==connectionGeneration.load() &&
              deviceConnected.load() && odysseySdReady()) {
            error=catalogue(total);
            if (error==OK) Serial.println("[SD] catalogue recovered without remount or FAT writes");
          }
        }
        // ENOMEM/EOVERFLOW means the BLE catalogue is incomplete, not that the
        // SD controller has stopped responding. Fail closed but keep FAT
        // mounted for the user's existing recordings.
        if (error==IO_ERROR) {
          noteSdMediaFault(7,catalogueStep,catalogueErrno);
          if (catalogueErrno!=ENOMEM && catalogueErrno!=EOVERFLOW) {
            errno=catalogueErrno?catalogueErrno:EIO;
            odysseySdMarkVfsFailure();
          }
        }
        break;
      case 8: total=catalogueBuffer.length();if(!total)error=FILE_UNAVAILABLE;break;
      case 14:
        invalidateBleReadCache();
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyRecoverSdCard("op14")?OK:NO_SD;
        break;
      case 17:
        invalidateBleReadCache();
        errno=0;
        error=removeFile(request.path);
        if (error==IO_ERROR) {
          const int removeErr=errno?errno:EIO;
          noteSdMediaFault(17,removeFailureStep,removeErr);
          errno=removeErr;
          odysseySdMarkVfsFailure();
        }
        break;
      case 18:
        invalidateBleReadCache();
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady())error=NO_SD;
        else total=clearRecordings();
        break;
      case 19: {
        invalidateBleReadCache();
        // Full-card FAT format is NOT Clear Recordings. PWA requires a
        // separate destructive confirmation; never retry automatically.
        selectedPath[0]=0;catalogueBuffer="";
        if (!storageReady()) { error=NO_SD;break; }
        // Historical or current write faults require non-destructive recovery,
        // not a FAT rebuild that could erase still-recoverable recordings.
        const uint8_t recordedFault=odysseyLastRecordFailureStage();
        if ((recordedFault>=40u && recordedFault!=43u && recordedFault!=48u &&
             recordedFault!=52u && recordedFault!=68u) ||
            odysseySdReadTimeoutQuarantined() ||
            odysseySdWriteTimeoutQuarantined()) { error=BUSY;break; }
        // No battery threshold is used as an SD mutation admission gate.
        // Format still requires explicit user token and healthy FAT state.
        const esp_err_t result=odysseyFormatSdCard();
        if (result!=ESP_OK) {
          error=IO_ERROR;
          noteSdMediaFault(19,1,int(result));
        } else {
          // The old recorder error belongs to files that the user explicitly
          // erased. Clear its NVS fault stage so the PWA sees live probe=6
          // instead of a stale stage-66 byte after a verified new FAT mount.
          odysseyPersistRecordFailure(0,0);
          sdLastMediaFaultOperation=0;
          sdLastMediaFaultStep=0;
          sdLastMediaFaultErrno=0;
        }
        break;
      }
      case 33: {
        // User-confirmed one-shot emergency FAT provisioning. The firmware
        // independently requires physical power-on and an unmountable FAT.
        invalidateBleReadCache();
        selectedPath[0]=0;catalogueBuffer="";
        const esp_err_t rescue=odysseySdEmergencyProvisionFat();
        if (rescue!=ESP_OK) {
          error=rescue==ESP_ERR_INVALID_STATE?BUSY:IO_ERROR;
          if (error==IO_ERROR) noteSdMediaFault(33,1,int(rescue));
        }
        break;
      }
      case 32: {
        // Operator-requested read-only FAT capacity and free space.
        uint64_t totalBytes=0,freeBytes=0,physicalBytes=0;
        const esp_err_t result=odysseySdGetCapacity(totalBytes,freeBytes,physicalBytes);
        if (result!=ESP_OK) {
          error=result==ESP_ERR_INVALID_STATE?NO_SD:IO_ERROR;
          if (error==IO_ERROR) {
            noteSdMediaFault(32,1,int(result));
            errno=EIO;
            odysseySdMarkVfsFailure();
          }
          break;
        }
        const int n=snprintf(reinterpret_cast<char*>(bytes),sizeof(bytes),
          "{\"version\":1,\"totalBytes\":%llu,\"freeBytes\":%llu,\"physicalBytes\":%llu}",
          static_cast<unsigned long long>(totalBytes),
          static_cast<unsigned long long>(freeBytes),
          static_cast<unsigned long long>(physicalBytes));
        if (n<0 || size_t(n)>=sizeof(bytes)) error=IO_ERROR;
        else size=size_t(n);
        break;
      }
      default:error=BAD_COMMAND;break;
    }
    if (request.operation==7 && (error==IO_ERROR || error==NO_SD)) {
      char detail[480];
      const int n=snprintf(detail,sizeof(detail),
        "{\"stage\":\"catalogue\",\"errno\":%d,\"sdState\":%u,\"sdProbe\":%u,\"espErr\":%ld,\"mountAttempts\":%lu,\"beginAttempts\":%lu,\"mountWhy\":%u,\"vfsStep\":%u,\"vfsErrno\":%ld,\"bbHigh\":%d,\"bbLow\":%d,\"raw0\":%u,\"rawFF\":%u,\"rawFE\":%u,\"rawOther\":%u,\"rawMaxFF\":%u,\"bbCmd12Candidate\":%d,\"bbReadIdle\":%u,\"bbDrain\":%lu,\"bbStop\":%u,\"bbCmd0\":%d,\"bbCmd8\":%d,\"bbR7\":%lu,\"wrE\":%lu,\"wrN\":%lu,\"wrX\":%lu,\"wrF\":%lu,\"wrD\":%lu}",
        catalogueErrno,unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
        static_cast<long>(odysseySdLastError()),static_cast<unsigned long>(odysseySdAttemptCount()),
        static_cast<unsigned long>(odysseySdBeginAttemptCount()),unsigned(odysseySdLastMountReasonCode()),
        unsigned(odysseySdVfsStepValue()),static_cast<long>(odysseySdVfsErrnoValue()),
        int(odysseySdBitBangCsHighState()),int(odysseySdBitBangCsLowState()),
        unsigned(odysseySdRawZeroCount()),unsigned(odysseySdRawFFCount()),
        unsigned(odysseySdRawFECount()),unsigned(odysseySdRawOtherCount()),
        unsigned(odysseySdRawMaxFFRunCount()),int(odysseySdBitBangCmd12Response()),
        unsigned(odysseySdBitBangCmd12ReadyState()),static_cast<unsigned long>(odysseySdBitBangDrainByteCount()),
        unsigned(odysseySdBitBangStopStateValue()),int(odysseySdBitBangCmd0Response()),
        int(odysseySdBitBangCmd8Response()),static_cast<unsigned long>(odysseySdBitBangR7Response()),
        static_cast<unsigned long>(odysseyLastWriteErrno()),
        static_cast<unsigned long>(odysseyLastWriteReturned()),
        static_cast<unsigned long>(odysseyLastWriteExpected()),
        static_cast<unsigned long>(odysseyLastWriteFerror()),
        static_cast<unsigned long>(odysseyLastDriverWriteFault()));
      reply(request,error,total,request.offset,reinterpret_cast<const uint8_t*>(detail),
        n>0?std::min(size_t(n),sizeof(detail)-1):0);
    } else reply(request,error,total,request.offset,bytes,size);
    sdBleLastTransferAt=millis();
    sdBleTransferInFlight=false;
  }
}

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length=characteristic->getLength();
    const uint8_t* p=characteristic->getData();
    if (!p || length<10 || length>75 || p[0]!=0xCA) return;
    Request request{};
    request.operation=p[1];
    memcpy(&request.id,p+2,4);
    memcpy(&request.offset,p+6,4);
    request.connection=connectionGeneration.load();
    // Op30: credit byte at 10. New op31: credit and pacing byte at 10/11.
    // The remaining path stays explicit, and existing command ABIs are unchanged.
    const bool adaptive=request.operation==31u;
    const bool fast=request.operation==30u || adaptive;
    if (fast && length<(adaptive?13u:12u)) return;
    // 16-byte compact opcode31 carries the token of the prior verified
    // selectFile request. A 4-byte suffix cannot be a valid /synap/ WAV path.
    request.compact=adaptive && length==16u;
    const size_t pathStart=request.compact?16u:(adaptive?12u:(fast?11u:10u));
    const size_t pathLength=length-pathStart;
    if (pathLength>63u) return;
    if (fast) request.credits=std::max(uint8_t(6),std::min(uint8_t(12),p[10]));
    if (adaptive) request.paceMs=std::max(uint8_t(8),std::min(uint8_t(15),p[11]));
    if (request.compact) memcpy(&request.sessionToken,p+12,4);
    if (pathLength) memcpy(request.path,p+pathStart,pathLength);
    request.path[pathLength]=0;
    if (!requests || xQueueSend(requests,&request,0)!=pdTRUE) reply(request,BUSY);
  }
};
class DataCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t value[496];size_t size=16;
    portENTER_CRITICAL(&responseMux);
    if (responseConnection==connectionGeneration.load()) {
      size=responseSize;memcpy(value,response,size);
    } else {
      memset(value,0,size);value[0]=0xCB;value[1]=1;
    }
    portEXIT_CRITICAL(&responseMux);
    characteristic->setValue(value,size);
  }
};

bool available(){return requests!=nullptr;}

void initialize() {
  requests=xQueueCreate(2,sizeof(Request));
  if (!requests || xTaskCreate(worker,"odyssey-sd",TRANSFER_STACK_BYTES,nullptr,1,nullptr)!=pdPASS) {
    if (requests) vQueueDelete(requests);
    requests=nullptr;
    Serial.println("[SD] BLE transfer worker unavailable");
    return;
  }
  Request initial{};initial.connection=connectionGeneration.load();reply(initial,OK);
}

void ble(BLEService* service) {
  // C3 retains media-v1 transport. Full-card format is available only through
  // explicit operation 19 with the PWA's destructive confirmation; never auto-format.
  auto* command=service->createCharacteristic("4fa12354-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  auto* data=service->createCharacteristic("4fa12355-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_READ);
  data->setCallbacks(new DataCallbacks());
  streamCharacteristic=service->createCharacteristic("4fa1235a-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  streamCharacteristic->addDescriptor(new BLE2902());
#endif
}
} // namespace OdysseyTransfer
#endif
