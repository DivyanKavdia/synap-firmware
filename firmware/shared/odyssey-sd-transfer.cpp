// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// C3 storage is mounted by the proven Arduino SPI/SD path and accessed through FAT/VFS.
// Files are deleted only after the PWA has imported and verified them.
bool odysseyFormatSdCard();
uint8_t odysseySdRecordFailureStage();
uint32_t odysseySdRecordLastBytes();
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0;
  uint8_t operation=0;
  char path[64]{};
};
static QueueHandle_t requests=nullptr;
static constexpr uint32_t TRANSFER_STACK_BYTES=8192;
static portMUX_TYPE responseMux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t response[496]{};
static size_t responseSize=16;
static uint32_t responseConnection=0;
static char selectedPath[64]{};
static String catalogueBuffer;
static int catalogueErrno=0;

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
static bool isSegmentedRecording(const char* path) {
  return path && strstr(path,"_p")!=nullptr;
}
// 1=complete and safe to expose, 0=incomplete/content-invalid, -1=filesystem I/O fault.
static int segmentedWavState(const char* fullPath,const struct stat& st) {
  const int journal=odysseyJournalPresence(fullPath);
  if (journal<0) return -1;
  if (journal>0) { errno=0;return 0; }
  errno=0;
  const int fd=open(fullPath,O_RDONLY);
  if (fd<0) return -1;
  uint8_t h[44]{};uint32_t audioBytes=0;
  const bool read=odysseyPreadAll(fd,h,sizeof(h),0);
  const int readError=read?0:(errno?errno:EIO);
  errno=0;
  const bool closed=close(fd)==0;
  const int closeError=closed?0:(errno?errno:EIO);
  if (!read || !closed) {
    errno=readError?readError:closeError;
    return -1;
  }
  if (!odysseyWavValid(h,audioBytes) || uint64_t(st.st_size)!=44ull+audioBytes) {
    errno=0;
    return 0;
  }
  errno=0;
  return 1;
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
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  errno=0;
  if (stat(full,&st)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
  if (!S_ISREG(st.st_mode) || st.st_size<=0) return FILE_UNAVAILABLE;
  if (isSegmentedRecording(path)) {
    const int state=segmentedWavState(full,st);
    if (state<0) return IO_ERROR;
    if (!state) return FILE_UNAVAILABLE;
  }
  if (uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

static uint8_t readSelected(const char* requestedPath,uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  // New clients make every chunk self-describing. Cache is scoped to path and
  // BLE generation; pread supports retransmission without shared seek state.
  const char* path=(requestedPath && requestedPath[0])?requestedPath:selectedPath;
  if (!path[0] || !strcmp(path,"@catalogue")) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    memcpy(bytes,catalogueBuffer.c_str()+offset,size);
    return OK;
  }
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  const uint32_t generation=connectionGeneration.load();
  if (odysseySdReadFd>=0 && (odysseySdReadConnection!=generation || strcmp(odysseySdReadPath,full)!=0))
    if (!odysseySdCloseReadLocked()) return IO_ERROR;
  if (odysseySdReadFd<0) {
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
    if (!S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
      return FILE_UNAVAILABLE;
    if (isSegmentedRecording(path)) {
      const int state=segmentedWavState(full,st);
      if (state<0) return IO_ERROR;
      if (!state) return FILE_UNAVAILABLE;
    }
    errno=0;
    odysseySdReadFd=open(full,O_RDONLY);
    if (odysseySdReadFd<0) return IO_ERROR;
    snprintf(odysseySdReadPath,sizeof(odysseySdReadPath),"%s",full);
    odysseySdReadConnection=generation;
    Serial.printf("[SD] transfer begin path=%s\n",path);
  }
  struct stat st{};
  errno=0;
  if (fstat(odysseySdReadFd,&st)!=0) {
    const int saved=errno?errno:EIO;(void)odysseySdCloseReadLocked();errno=saved;return IO_ERROR;
  }
  if (st.st_size<0 || uint64_t(st.st_size)>UINT32_MAX) {
    (void)odysseySdCloseReadLocked();return FILE_UNAVAILABLE;
  }
  total=uint32_t(st.st_size);
  if (offset>=total) { (void)odysseySdCloseReadLocked();return FILE_UNAVAILABLE; }
  size=std::min(size_t(480),size_t(total-offset));
  const bool ok=odysseyPreadAll(odysseySdReadFd,bytes,size,off_t(offset));
  odysseySdReadAt=millis();
  if (!ok || uint64_t(offset)+size==total) {
    const bool closed=odysseySdCloseReadLocked();
    if (!ok || !closed) return IO_ERROR;
  }
  return OK;
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  catalogueErrno=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  errno=0;
  if (!odysseySdCloseReadLocked()) {
    catalogueErrno=errno?errno:EIO;
    return IO_ERROR;
  }

  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) {
    catalogueErrno=EINVAL; return IO_ERROR;
  }
  DIR* directory=opendir(directoryPath);
  if (!directory) {
    catalogueErrno=errno;
    Serial.printf("[SD] catalogue opendir failed errno=%d path=%s\n",catalogueErrno,directoryPath);
    return IO_ERROR;
  }

  if (!catalogueBuffer.reserve(2048)) {
    catalogueErrno=ENOMEM;closedir(directory);return IO_ERROR;
  }
  catalogueBuffer="[";
  unsigned count=0;
  for (;;) {
    errno=0;
    dirent* entry=readdir(directory);
    if (!entry) {
      if (errno) catalogueErrno=errno;
      break;
    }
    if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    char logical[64];
    const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
    char full[96];
    if (!odysseySdPath(logical,full,sizeof(full))) continue;
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) {
      if (errno==ENOENT) continue;
      catalogueErrno=errno?errno:EIO;
      break;
    }
    if (!S_ISREG(st.st_mode) || st.st_size<0 || uint64_t(st.st_size)>UINT32_MAX) continue;
    if (isSegmentedRecording(logical)) {
      const int state=segmentedWavState(full,st);
      if (state<0) { catalogueErrno=errno?errno:EIO;break; }
      if (!state) continue;
    }
    if (count++) catalogueBuffer+=",";
    catalogueBuffer+="{\"path\":\""+String(logical)+"\",\"bytes\":"+String(uint32_t(st.st_size))+"}";
    if (count>=100) break;
  }
  if (closedir(directory)!=0 && !catalogueErrno) catalogueErrno=errno;
  if (catalogueErrno) {
    catalogueBuffer="";
    Serial.printf("[SD] catalogue readdir/closedir failed errno=%d\n",catalogueErrno);
    return IO_ERROR; // Never send a silently truncated catalogue after an I/O fault.
  }
  catalogueBuffer+="]";
  total=catalogueBuffer.length();
  return OK;
}

static uint8_t removeFileLocked(const char* path) {
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  errno=0;
  if (stat(full,&st)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
  if (!S_ISREG(st.st_mode)) return FILE_UNAVAILABLE;
  errno=0;
  if (!odysseySdCloseReadLocked()) return IO_ERROR;
  if (!odysseyRemoveJournal(full)) return IO_ERROR;
  errno=0;
  if (unlink(full)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
  // Verify metadata visibility before telling the PWA it may forget its source.
  errno=0;
  if (stat(full,&st)==0) { errno=EIO;return IO_ERROR; }
  if (errno!=ENOENT) return IO_ERROR;
  errno=0;
  return OK;
}

static uint8_t removeFile(const char* path) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  return removeFileLocked(path);
}

static uint8_t clearRecordings(uint32_t& removed) {
  removed=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  errno=0;
  if (!odysseySdCloseReadLocked()) return IO_ERROR;
  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) { errno=EINVAL;return IO_ERROR; }
  errno=0;
  DIR* directory=opendir(directoryPath);
  if (!directory) return IO_ERROR;

  String logicalPaths[100];
  uint16_t count=0;
  int scanError=0;
  for (;;) {
    errno=0;
    dirent* entry=readdir(directory);
    if (!entry) { if (errno) scanError=errno;break; }
    if (count>=100) break;
    if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    char logical[64];
    const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
    char full[96];
    if (!odysseySdPath(logical,full,sizeof(full))) { scanError=EINVAL;break; }
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) {
      if (errno==ENOENT) continue;
      scanError=errno?errno:EIO;break;
    }
    if (S_ISREG(st.st_mode)) logicalPaths[count++]=logical;
  }
  if (closedir(directory)!=0 && !scanError) scanError=errno?errno:EIO;
  if (scanError) { errno=scanError;return IO_ERROR; }

  for (uint16_t i=0;i<count;++i) {
    const uint8_t result=removeFileLocked(logicalPaths[i].c_str());
    if (result==OK) ++removed;
    else if (result!=FILE_UNAVAILABLE) return result;
  }
  errno=0;
  return OK;
}

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
    {
      OdysseySdGuard guard(0);
      if (guard && odysseySdReadFd>=0 && (!deviceConnected.load() ||
          odysseySdReadConnection!=connectionGeneration.load() ||
          uint32_t(millis()-odysseySdReadAt)>=15000u || odysseyRecording.load() || sleepPending)) {
        if (!odysseySdCloseReadLocked()) odysseySdMarkVfsFailure(errno);
      }
    }

    if (xQueueReceive(requests,&request,pdMS_TO_TICKS(500))!=pdTRUE) {
      // Never remount merely because the worker is idle or catalogue failed.
      // A physical disconnected double-tap is an explicit recovery request,
      // just like PWA operation 14, and may safely run while storage is idle.
      const bool idleEnough=!odysseyRecording.load() && !streamingEnabled.load() &&
        !otaBusy() && !sleepPending;
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
    // Connected remote standby only idles the microphone/CPU; SD media must
    // remain readable for verified sync and recovery without a forced wake.
    if (odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending) {
      reply(request,BUSY);continue;
    }
    uint8_t error=OK;uint32_t total=0;size_t size=0;
    switch (request.operation) {
      case 3:
        error=selectFile(request.path,total);
        if (error==IO_ERROR) odysseySdMarkVfsFailure(errno);
        break;
      case 4:
        error=readSelected(request.path,request.offset,total,bytes,size);
        if (error==IO_ERROR) odysseySdMarkVfsFailure(errno);
        break;
      case 7:
        error=catalogue(total);
        // Never auto-unmount/remount a mounted card because a catalogue read
        // failed. Preserve the observed state for diagnosis; explicit op 14 is
        // the only connected remount path.
        if (error==IO_ERROR) odysseySdMarkVfsFailure(catalogueErrno?catalogueErrno:errno);
        break;
      case 8: total=catalogueBuffer.length();if(!total)error=FILE_UNAVAILABLE;break;
      case 14:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyRecoverSdCard("op14")?OK:NO_SD;
        break;
      case 17:
        error=removeFile(request.path);
        if (error==IO_ERROR) odysseySdMarkVfsFailure(errno);
        break;
      case 18:
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady()) error=NO_SD;
        else {
          error=clearRecordings(total);
          if (error==IO_ERROR) odysseySdMarkVfsFailure(errno);
        }
        break;
      case 19:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyFormatSdCard()?OK:IO_ERROR;
        break;
      default:error=BAD_COMMAND;break;
    }
    if (request.operation==7 && (error==IO_ERROR || error==NO_SD)) {
      char detail[480];
      const int n=snprintf(detail,sizeof(detail),
        "{\"stage\":\"catalogue\",\"errno\":%d,\"sdState\":%u,\"sdProbe\":%u,\"espErr\":%ld,\"ioErrno\":%ld,\"releaseErr\":%ld,\"mountAttempts\":%lu,\"beginAttempts\":%lu,\"releaseAttempts\":%lu,\"mountWhy\":%u,\"recordStage\":%u,\"recordBytes\":%lu,\"lastRecordStage\":%lu,\"lastRecordErrno\":%ld,\"lastRecordBytes\":%lu,\"lastRecordBuild\":%lu}",
        catalogueErrno,unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
        static_cast<long>(odysseySdLastError()),static_cast<long>(odysseySdLastIoError()),
        static_cast<long>(odysseySdLastReleaseErrorCode()),static_cast<unsigned long>(odysseySdAttemptCount()),
        static_cast<unsigned long>(odysseySdBeginAttemptCount()),static_cast<unsigned long>(odysseySdReleaseAttemptCount()),
        unsigned(odysseySdLastMountReasonCode()),unsigned(odysseySdRecordFailureStage()),
        static_cast<unsigned long>(odysseySdRecordLastBytes()),static_cast<unsigned long>(odysseyStoredStage),
        static_cast<long>(odysseyStoredErrno),static_cast<unsigned long>(odysseyStoredBytes),
        static_cast<unsigned long>(odysseyStoredBuild));
      reply(request,error,total,request.offset,reinterpret_cast<const uint8_t*>(detail),
        n>0?std::min(size_t(n),sizeof(detail)-1):0);
    } else reply(request,error,total,request.offset,bytes,size);
  }
}

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length=characteristic->getLength();
    const uint8_t* p=characteristic->getData();
    if (!p || length<10 || length>73 || p[0]!=0xCA) return;
    Request request{};
    request.operation=p[1];
    memcpy(&request.id,p+2,4);
    memcpy(&request.offset,p+6,4);
    request.connection=connectionGeneration.load();
    const size_t pathLength=length-10;
    if (pathLength) memcpy(request.path,p+10,pathLength);
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
  auto* command=service->createCharacteristic("4fa12354-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  auto* data=service->createCharacteristic("4fa12355-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_READ);
  data->setCallbacks(new DataCallbacks());
}
} // namespace OdysseyTransfer
#endif
