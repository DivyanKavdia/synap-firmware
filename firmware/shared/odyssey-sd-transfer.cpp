// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// Files are never deleted by transfer itself. The PWA first imports and verifies
// the complete WAV, then explicitly sends operation 17 for that path.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0;
  uint8_t operation=0;
  char path[64]{};
};
static QueueHandle_t requests=nullptr;
// FAT/VFS directory enumeration has a materially deeper call stack than normal BLE work on C3.
// Keep this worker at parity with the proven Chakshu transfer task so an SD catalogue cannot
// overflow the task stack and reset the single-core C3 during the first post-connect probe.
static constexpr uint32_t TRANSFER_STACK_BYTES=8192;
static portMUX_TYPE responseMux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t response[496]{};
static size_t responseSize=16;
static uint32_t responseConnection=0;
static char selectedPath[64]{};
static String catalogueBuffer;

enum : uint8_t {
  OK=0, BUSY=1, BAD_COMMAND=2, NO_SD=3, IO_ERROR=7, FILE_UNAVAILABLE=11
};

static bool safeWavPath(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12 || n>63 || strcmp(path+n-4,".wav")!=0) return false;
  for (size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}

static bool storageReady() {
  // Normal PWA reload/reconnect is observational only: never remount here.
  return odysseySdDetectionState()==1 && SD.cardType()!=CARD_NONE;
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
  if (!storageReady()) return NO_SD;
  if (!SD.exists(path)) return FILE_UNAVAILABLE;
  File file=SD.open(path,FILE_READ);
  if (!file || file.isDirectory()) { if(file)file.close(); return FILE_UNAVAILABLE; }
  total=file.size();
  file.close();
  if (!total) return FILE_UNAVAILABLE;
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

static uint8_t readSelected(uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  if (selectedPath[0]) {
    if (!storageReady()) return NO_SD;
    if (!SD.exists(selectedPath)) return FILE_UNAVAILABLE;
    File file=SD.open(selectedPath,FILE_READ);
    if (!file || file.isDirectory()) { if(file)file.close(); return FILE_UNAVAILABLE; }
    total=file.size();
    if (offset>=total) { file.close(); return FILE_UNAVAILABLE; }
    size=std::min(size_t(480),size_t(total-offset));
    if (!file.seek(offset) || file.read(bytes,size)!=int(size)) { file.close(); return IO_ERROR; }
    file.close();
    return OK;
  }
  total=catalogueBuffer.length();
  if (!total || offset>=total) return FILE_UNAVAILABLE;
  size=std::min(size_t(480),size_t(total-offset));
  memcpy(bytes,catalogueBuffer.c_str()+offset,size);
  return OK;
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  if (!storageReady()) return NO_SD;
  File directory=SD.open("/synap");
  if (!directory) return IO_ERROR;
  catalogueBuffer.reserve(2048);
  catalogueBuffer="[";
  unsigned count=0;
  for (File entry=directory.openNextFile();entry;entry=directory.openNextFile()) {
    const String path=entry.path();
    const bool include=!entry.isDirectory() && safeWavPath(path.c_str());
    const size_t bytes=entry.size();
    entry.close();
    if (!include) continue;
    if (count++) catalogueBuffer+=",";
    catalogueBuffer+="{\"path\":\""+path+"\",\"bytes\":"+String(bytes)+"}";
    if (count>=100) break;
  }
  directory.close();
  catalogueBuffer+="]";
  total=catalogueBuffer.length();
  return total?OK:IO_ERROR;
}

static uint8_t removeFile(const char* path) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  if (!storageReady()) return NO_SD;
  if (!SD.exists(path)) return FILE_UNAVAILABLE;
  return SD.remove(path)?OK:IO_ERROR;
}

static uint16_t clearRecordings() {
  if (!storageReady()) return 0;
  File directory=SD.open("/synap");
  if (!directory) return 0;
  String paths[100];
  uint16_t count=0;
  for (File entry=directory.openNextFile();entry && count<100;entry=directory.openNextFile()) {
    const String path=entry.path();
    const bool include=!entry.isDirectory() && safeWavPath(path.c_str());
    entry.close();
    if (include) paths[count++]=path;
  }
  directory.close();
  uint16_t removed=0;
  for (uint16_t i=0;i<count;++i) if (SD.remove(paths[i].c_str())) ++removed;
  return removed;
}

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
    if (xQueueReceive(requests,&request,portMAX_DELAY)!=pdTRUE) continue;
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load()) continue;
    if (odysseyRecording.load() || streamingEnabled.load() || otaBusy() || remoteStandby || sleepPending) {
      reply(request,BUSY);continue;
    }
    uint8_t error=OK;uint32_t total=0;size_t size=0;
    switch (request.operation) {
      case 3: error=selectFile(request.path,total); break;
      case 4: error=readSelected(request.offset,total,bytes,size); break;
      case 7: error=catalogue(total); break;
      case 8: total=catalogueBuffer.length(); if(!total)error=FILE_UNAVAILABLE; break;
      case 14:
        selectedPath[0]=0;catalogueBuffer="";
        // Explicit Settings recovery is the only connected-path remount.
        error=odysseyRecoverSdCard()?OK:NO_SD;break;
      case 17: error=removeFile(request.path); break;
      case 18:
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady())error=NO_SD;else total=clearRecordings();
        break;
      default: error=BAD_COMMAND; break;
    }
    reply(request,error,total,request.offset,bytes,size);
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

bool available() { return requests!=nullptr; }

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
