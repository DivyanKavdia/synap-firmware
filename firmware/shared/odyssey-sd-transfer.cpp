// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// C3 storage is mounted through the stock Arduino SD SPI path and accessed through FAT/VFS.
// Files are deleted only after the PWA has imported and verified them.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0,windowEpoch=0;
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
static BLECharacteristic* streamCharacteristic=nullptr;
static std::atomic<uint32_t> cancelWindow{0},streamNotifyRejected{0};

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
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0) return FILE_UNAVAILABLE;
  if (uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

static uint8_t readSelected(const char* requestedPath,uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  // New clients make every chunk self-describing. This prevents catalogue
  // refreshes or another client from replacing the selected file mid-transfer.
  if (requestedPath && requestedPath[0]) {
    if (!strcmp(requestedPath,"@catalogue")) {
      total=catalogueBuffer.length();
      if (!total || offset>=total) return FILE_UNAVAILABLE;
      size=std::min(size_t(480),size_t(total-offset));
      memcpy(bytes,catalogueBuffer.c_str()+offset,size);
      return OK;
    }
    if (!safeWavPath(requestedPath)) return BAD_COMMAND;
    if (offset==0) Serial.printf("[SD] transfer begin path=%s\n",requestedPath);
    OdysseySdGuard guard;
    if (!guard || !storageReady()) return NO_SD;
    char full[96];
    if (!fullPath(requestedPath,full,sizeof(full))) return BAD_COMMAND;
    struct stat st{};
    if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
      return FILE_UNAVAILABLE;
    total=uint32_t(st.st_size);
    if (offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    FILE* file=fopen(full,"rb");
    if (!file) return FILE_UNAVAILABLE;
    const bool ok=fseek(file,long(offset),SEEK_SET)==0 && fread(bytes,1,size,file)==size;
    fclose(file);
    return ok?OK:IO_ERROR;
  }

  // Legacy clients still use the selected-file/catalogue state.
  if (!selectedPath[0]) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    memcpy(bytes,catalogueBuffer.c_str()+offset,size);
    return OK;
  }

  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(selectedPath,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
    return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  if (offset>=total) return FILE_UNAVAILABLE;
  size=std::min(size_t(480),size_t(total-offset));

  FILE* file=fopen(full,"rb");
  if (!file) return FILE_UNAVAILABLE;
  const bool ok=fseek(file,long(offset),SEEK_SET)==0 && fread(bytes,1,size,file)==size;
  fclose(file);
  return ok?OK:IO_ERROR;
}


class StreamCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic*,Status status,uint32_t) override {
    if (status!=SUCCESS_NOTIFY) ++streamNotifyRejected;
  }
};

static bool sendMediaPacket(const Request& request,uint8_t kind,uint8_t error,uint32_t total,
                            uint32_t offset,const uint8_t* bytes,size_t size) {
  if (!streamCharacteristic || !deviceConnected.load() ||
      request.connection!=connectionGeneration.load()) return false;
  const uint16_t capacity=attValueCapacity.load();
  if (capacity<16 || size>480 || 16u+size>capacity) return false;
  static uint8_t packet[496];
  packet[0]=0xCC;packet[1]=1;packet[2]=kind;packet[3]=error;
  put32le(packet+4,request.id);put32le(packet+8,total);put32le(packet+12,offset);
  if (size && bytes) memcpy(packet+16,bytes,size);
  streamCharacteristic->setValue(packet,16+size);
  for (uint8_t attempt=0;attempt<4;++attempt) {
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load() ||
        request.windowEpoch!=cancelWindow.load()) return false;
    if (attempt) vTaskDelay(pdMS_TO_TICKS(8u*attempt));
    const uint32_t rejectedBefore=streamNotifyRejected.load();
    streamCharacteristic->notify();
    if (streamNotifyRejected.load()==rejectedBefore) return true;
  }
  return false;
}

static void endMediaWindow(const Request& request,uint8_t error,uint32_t total,uint32_t offset) {
  for (uint8_t attempt=0;attempt<6 && request.windowEpoch==cancelWindow.load();++attempt) {
    if (sendMediaPacket(request,2,error,total,offset,nullptr,0)) return;
    vTaskDelay(pdMS_TO_TICKS(12));
  }
}

static void streamWindow(const Request& request) {
  // Credit-window protocol shared with Chakshu: no more than eight data
  // notifications are outstanding for a single command. The app requests the
  // next byte offset after the end marker, so reconnect/resume is naturally
  // path/offset based.
  const uint16_t att=attValueCapacity.load();
  const size_t capacity=att>16 ? std::min(size_t(480),size_t(att-16)) : 0;
  if (capacity<32) {
    // Let the app observe an empty window three times and fall back to media-v1.
    endMediaWindow(request,OK,0,request.offset);
    return;
  }

  uint32_t offset=request.offset,total=0;
  uint8_t error=OK;
  bool storageFault=false;
  if (!selectedPath[0]) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) error=FILE_UNAVAILABLE;
    for (uint8_t count=0;!error && count<8 && offset<total;++count) {
      if (request.windowEpoch!=cancelWindow.load() ||
          request.connection!=connectionGeneration.load() || !deviceConnected.load()) return;
      const size_t size=std::min(capacity,size_t(total-offset));
      if (!sendMediaPacket(request,1,OK,total,offset,
          reinterpret_cast<const uint8_t*>(catalogueBuffer.c_str()+offset),size)) break;
      offset+=size;
      vTaskDelay(pdMS_TO_TICKS(6));
    }
  } else {
    {
      OdysseySdGuard guard(pdMS_TO_TICKS(2000));
      if (!guard || !storageReady()) error=NO_SD;
      char full[96]{};
      struct stat st{};
      FILE* file=nullptr;
      if (!error && !fullPath(selectedPath,full,sizeof(full))) error=BAD_COMMAND;
      if (!error && (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 ||
          uint64_t(st.st_size)>UINT32_MAX)) error=FILE_UNAVAILABLE;
      if (!error) {
        total=uint32_t(st.st_size);
        if (offset>=total) error=FILE_UNAVAILABLE;
      }
      if (!error) {
        file=fopen(full,"rb");
        if (!file) error=FILE_UNAVAILABLE;
      }
      if (!error && fseek(file,long(offset),SEEK_SET)!=0) {
        error=IO_ERROR;storageFault=true;
      }
      uint8_t bytes[480];
      for (uint8_t count=0;!error && count<8 && offset<total;++count) {
        if (request.windowEpoch!=cancelWindow.load() ||
            request.connection!=connectionGeneration.load() || !deviceConnected.load()) {
          if (file) fclose(file);
          return;
        }
        const size_t size=std::min(capacity,size_t(total-offset));
        if (fread(bytes,1,size,file)!=size) {
          error=IO_ERROR;storageFault=true;break;
        }
        if (!sendMediaPacket(request,1,OK,total,offset,bytes,size)) break;
        offset+=size;
        vTaskDelay(pdMS_TO_TICKS(6));
      }
      if (file && fclose(file)!=0) { error=IO_ERROR;storageFault=true; }
      if (storageFault) odysseySdMarkVfsFailure();
    }
    // Never quiesce while the storage guard is held.
    if (storageFault) (void)odysseySdQuiesceFaultedSession(750u);
  }
  endMediaWindow(request,error,total,offset);
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  catalogueErrno=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;

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
    if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0) continue;
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

static uint8_t removeFile(const char* path) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode)) return FILE_UNAVAILABLE;
  return unlink(full)==0?OK:IO_ERROR;
}

static uint16_t clearRecordings() {
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

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
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
      case 3: error=selectFile(request.path,total); break;
      case 12: streamWindow(request); continue;
      case 4:
        error=readSelected(request.path,request.offset,total,bytes,size);
        if (error==IO_ERROR) {
          odysseySdMarkVfsFailure();
          // Cleanup only. Do not auto-remount a connected transfer behind the
          // PWA; just ensure this failed read cannot strand the card protocol.
          (void)odysseySdQuiesceFaultedSession(750u);
        }
        break;
      case 7:
        error=catalogue(total);
        if (error==IO_ERROR) {
          odysseySdMarkVfsFailure();
          // Same rule for catalogue EIO: terminate the mounted card session,
          // leaving explicit Retry SD as the connected remount action.
          (void)odysseySdQuiesceFaultedSession(750u);
        }
        break;
      case 8: total=catalogueBuffer.length();if(!total)error=FILE_UNAVAILABLE;break;
      case 14:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyRecoverSdCard("op14")?OK:NO_SD;
        break;
      case 17: error=removeFile(request.path); break;
      case 18:
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady())error=NO_SD;else total=clearRecordings();
        break;
      default:error=BAD_COMMAND;break;
    }
    if (request.operation==7 && (error==IO_ERROR || error==NO_SD)) {
      char detail[480];
      const int n=snprintf(detail,sizeof(detail),
        "{\"stage\":\"catalogue\",\"errno\":%d,\"sdState\":%u,\"sdProbe\":%u,\"espErr\":%ld,\"mountAttempts\":%lu,\"beginAttempts\":%lu,\"mountWhy\":%u,\"bbHigh\":%d,\"bbLow\":%d,\"raw0\":%u,\"rawFF\":%u,\"rawFE\":%u,\"rawOther\":%u,\"rawMaxFF\":%u,\"bbCmd12Candidate\":%d,\"bbReadIdle\":%u,\"bbDrain\":%lu,\"bbStop\":%u,\"bbCmd0\":%d,\"bbCmd8\":%d,\"bbR7\":%lu}",
        catalogueErrno,unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
        static_cast<long>(odysseySdLastError()),static_cast<unsigned long>(odysseySdAttemptCount()),
        static_cast<unsigned long>(odysseySdBeginAttemptCount()),unsigned(odysseySdLastMountReasonCode()),
        int(odysseySdBitBangCsHighState()),int(odysseySdBitBangCsLowState()),
        unsigned(odysseySdRawZeroCount()),unsigned(odysseySdRawFFCount()),
        unsigned(odysseySdRawFECount()),unsigned(odysseySdRawOtherCount()),
        unsigned(odysseySdRawMaxFFRunCount()),int(odysseySdBitBangCmd12Response()),
        unsigned(odysseySdBitBangCmd12ReadyState()),static_cast<unsigned long>(odysseySdBitBangDrainByteCount()),
        unsigned(odysseySdBitBangStopStateValue()),int(odysseySdBitBangCmd0Response()),
        int(odysseySdBitBangCmd8Response()),static_cast<unsigned long>(odysseySdBitBangR7Response()));
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
    request.windowEpoch=cancelWindow.load();
    if (request.operation==16) {
      ++cancelWindow;
      reply(request,OK);
      return;
    }
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
bool streamAvailable(){return requests!=nullptr && streamCharacteristic!=nullptr;}

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
  streamCharacteristic=service->createCharacteristic("4fa1235a-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_NOTIFY);
  streamCharacteristic->setCallbacks(new StreamCallbacks());
#if defined(CONFIG_BLUEDROID_ENABLED)
  streamCharacteristic->addDescriptor(new BLE2902());
#endif
}
} // namespace OdysseyTransfer
#endif
