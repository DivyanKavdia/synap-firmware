// Odyssey C3 SD media: verified BLE transfer plus direct Wi-Fi cloud upload.
// C3 storage is mounted through the stock Arduino SD SPI path and accessed through FAT/VFS.
// Files are deleted only after the destination has durably accepted and verified them.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <WiFi.h>
#include <esp_http_client.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
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

static constexpr uint32_t WIFI_SEGMENT_MS=120000;
static constexpr uint32_t WIFI_SEGMENT_PCM_BYTES=(SAMPLE_RATE*2u*WIFI_SEGMENT_MS)/1000u;
static constexpr size_t WIFI_STAGE_CAPACITY=2304;
static char wifiStage[WIFI_STAGE_CAPACITY]{};
static size_t wifiStageLength=0;
static std::atomic<bool> wifiUploadActive{false},wifiProfileConfigured{false};
static portMUX_TYPE wifiStatusMux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t wifiPhase=0; // 0 idle,1 connecting,2 resuming,3 uploading,4 finalizing,5 done,6 error
static uint32_t wifiSegment=0,wifiSegmentCount=0,wifiUploadedBytes=0,wifiTotalBytes=0;
static int wifiHttpStatus=0,wifiErrorCode=0;
static char wifiRecordingId[40]{},wifiStatusMessage[72]{};

struct WifiJob {
  char endpoint[192]{};
  char token[896]{};
  char recordingId[40]{};
  char logicalPath[64]{};
};
static WifiJob wifiJob{};

enum : uint8_t {
  OK=0,BUSY=1,BAD_COMMAND=2,NO_SD=3,IO_ERROR=7,FILE_UNAVAILABLE=11
};

static void wifiSetStatus(uint8_t phase,const char* message,int httpStatus=0,int errorCode=0) {
  portENTER_CRITICAL(&wifiStatusMux);
  wifiPhase=phase;wifiHttpStatus=httpStatus;wifiErrorCode=errorCode;
  snprintf(wifiStatusMessage,sizeof(wifiStatusMessage),"%s",message?message:"");
  portEXIT_CRITICAL(&wifiStatusMux);
}

static bool wifiLoadProfile(char* ssid,size_t ssidCap,char* password,size_t passwordCap) {
  Preferences prefs;
  if (!prefs.begin("synapwifi",true)) return false;
  const String savedSsid=prefs.getString("ssid","");
  const String savedPassword=prefs.getString("pass","");
  prefs.end();
  if (!savedSsid.length() || savedSsid.length()>32 || savedPassword.length()>63) return false;
  snprintf(ssid,ssidCap,"%s",savedSsid.c_str());
  snprintf(password,passwordCap,"%s",savedPassword.c_str());
  return true;
}

static bool wifiSaveProfile(const char* ssid,const char* password) {
  if (!ssid || !ssid[0] || strlen(ssid)>32 || !password || strlen(password)>63) return false;
  Preferences prefs;
  if (!prefs.begin("synapwifi",false)) return false;
  const bool ok=prefs.putString("ssid",ssid)>0 &&
    (password[0] ? prefs.putString("pass",password)>0 : (prefs.remove("pass"),true));
  prefs.end();
  wifiProfileConfigured=ok;
  return ok;
}

static void wifiForgetProfile() {
  Preferences prefs;
  if (prefs.begin("synapwifi",false)) { prefs.clear();prefs.end(); }
  wifiProfileConfigured=false;
}

static int hexNibble(char c) {
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return c-'a'+10;
  if (c>='A'&&c<='F') return c-'A'+10;
  return -1;
}

static bool stageField(char key,char* out,size_t capacity,bool required=true) {
  if (!out || capacity<1) return false;
  out[0]=0;
  const char prefix[3]={key,'=',0};
  const char* begin=wifiStage;
  const char* found=nullptr;
  while ((found=strstr(begin,prefix))) {
    if (found==wifiStage || found[-1]=='\n') break;
    begin=found+1;
  }
  if (!found) return !required;
  found+=2;
  const char* end=strchr(found,'\n');
  if (!end) end=wifiStage+wifiStageLength;
  const size_t hexLength=size_t(end-found);
  if ((hexLength&1u) || hexLength/2>=capacity) return false;
  for (size_t i=0;i<hexLength;i+=2) {
    const int hi=hexNibble(found[i]),lo=hexNibble(found[i+1]);
    if (hi<0 || lo<0) return false;
    out[i/2]=char((hi<<4)|lo);
  }
  out[hexLength/2]=0;
  return true;
}

static uint8_t wifiStageChunk(uint32_t offset,const char* chunk,uint32_t& total) {
  const size_t n=chunk?strlen(chunk):0;
  if (offset==0) { memset(wifiStage,0,sizeof(wifiStage));wifiStageLength=0; }
  if (offset!=wifiStageLength || !n || wifiStageLength+n>=sizeof(wifiStage)) return BAD_COMMAND;
  memcpy(wifiStage+wifiStageLength,chunk,n);
  wifiStageLength+=n;wifiStage[wifiStageLength]=0;total=uint32_t(wifiStageLength);
  return OK;
}

static bool secureEndpoint(const char* endpoint) {
  if (!endpoint || strncmp(endpoint,"https://",8)!=0 || strlen(endpoint)>180) return false;
  return !strchr(endpoint,'?') && !strchr(endpoint,'#') && !strchr(endpoint,'@');
}

static bool jsonUInt(const char* json,const char* key,uint32_t& value) {
  if (!json || !key) return false;
  char pattern[48];snprintf(pattern,sizeof(pattern),"\"%s\":",key);
  const char* p=strstr(json,pattern);if(!p)return false;p+=strlen(pattern);
  while (*p==' '||*p=='\t')++p;
  if (*p<'0'||*p>'9') return false;
  uint64_t v=0;while(*p>='0'&&*p<='9'){v=v*10u+uint32_t(*p-'0');if(v>UINT32_MAX)return false;++p;}
  value=uint32_t(v);return true;
}

static bool jsonBool(const char* json,const char* key,bool& value) {
  if (!json || !key) return false;
  char pattern[48];snprintf(pattern,sizeof(pattern),"\"%s\":",key);
  const char* p=strstr(json,pattern);if(!p)return false;p+=strlen(pattern);
  while (*p==' '||*p=='\t')++p;
  if (!strncmp(p,"true",4)){value=true;return true;}
  if (!strncmp(p,"false",5)){value=false;return true;}
  return false;
}

static esp_http_client_handle_t wifiHttp(const char* url,esp_http_client_method_t method,const char* token) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  esp_http_client_config_t cfg={};
  cfg.url=url;cfg.timeout_ms=30000;cfg.keep_alive_enable=true;cfg.crt_bundle_attach=esp_crt_bundle_attach;
  auto client=esp_http_client_init(&cfg);
  if (!client) return nullptr;
  esp_http_client_set_method(client,method);
  char authorization[960];
  const int n=snprintf(authorization,sizeof(authorization),"SynapDevice %s",token?token:"");
  if (n<=0 || size_t(n)>=sizeof(authorization)) { esp_http_client_cleanup(client);return nullptr; }
  esp_http_client_set_header(client,"Authorization",authorization);
  esp_http_client_set_header(client,"User-Agent","Synap-Odyssey-C3/1");
  return client;
#else
  (void)url;(void)method;(void)token;
  return nullptr;
#endif
}

static bool httpWriteAll(esp_http_client_handle_t client,const uint8_t* bytes,size_t size) {
  size_t written=0;
  while (written<size) {
    const int n=esp_http_client_write(client,reinterpret_cast<const char*>(bytes+written),int(size-written));
    if (n<=0) return false;
    written+=size_t(n);
  }
  return true;
}

static int httpFinish(esp_http_client_handle_t client,char* response,size_t capacity) {
  if (!client) return -1;
  if (esp_http_client_fetch_headers(client)<0) { esp_http_client_close(client);esp_http_client_cleanup(client);return -1; }
  const int status=esp_http_client_get_status_code(client);
  if (response && capacity) {
    const int n=esp_http_client_read_response(client,response,int(capacity-1));
    response[n>0?size_t(n):0]=0;
  }
  esp_http_client_close(client);esp_http_client_cleanup(client);
  return status;
}

static int httpJson(const char* url,esp_http_client_method_t method,const char* token,
                    const char* body,char* response,size_t capacity) {
  auto client=wifiHttp(url,method,token);if(!client)return -1;
  const size_t bytes=body?strlen(body):0;
  if (body) esp_http_client_set_header(client,"Content-Type","application/json");
  if (esp_http_client_open(client,int(bytes))!=ESP_OK) { esp_http_client_cleanup(client);return -1; }
  if (bytes && !httpWriteAll(client,reinterpret_cast<const uint8_t*>(body),bytes)) {
    esp_http_client_close(client);esp_http_client_cleanup(client);return -1;
  }
  return httpFinish(client,response,capacity);
}


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


static int uploadWavSegment(FILE* file,const WifiJob& job,uint32_t index,uint32_t pcmOffset,
                            uint32_t pcmBytes,uint32_t startMs,uint32_t endMs) {
  char url[320],indexText[16],startText[24],endText[24];
  snprintf(url,sizeof(url),"%s/v1/device-uploads/%s/segments/%lu",job.endpoint,job.recordingId,
    static_cast<unsigned long>(index));
  auto client=wifiHttp(url,HTTP_METHOD_PUT,job.token);if(!client)return -1;
  esp_http_client_set_header(client,"Content-Type","audio/wav");
  snprintf(indexText,sizeof(indexText),"%lu",static_cast<unsigned long>(index));
  snprintf(startText,sizeof(startText),"%lu",static_cast<unsigned long>(startMs));
  snprintf(endText,sizeof(endText),"%lu",static_cast<unsigned long>(endMs));
  esp_http_client_set_header(client,"X-Synap-Start-Ms",startText);
  esp_http_client_set_header(client,"X-Synap-End-Ms",endText);
  const uint32_t bodyBytes=44u+pcmBytes;
  if (esp_http_client_open(client,int(bodyBytes))!=ESP_OK) { esp_http_client_cleanup(client);return -1; }
  uint8_t header[44];::odysseyWavHeader(header,pcmBytes);
  bool ok=httpWriteAll(client,header,sizeof(header));
  if (ok && fseek(file,long(44u+pcmOffset),SEEK_SET)!=0) ok=false;
  uint8_t buffer[4096];
  uint32_t remaining=pcmBytes;
  while (ok && remaining) {
    const size_t chunk=std::min(size_t(remaining),sizeof(buffer));
    if (fread(buffer,1,chunk,file)!=chunk || !httpWriteAll(client,buffer,chunk)) { ok=false;break; }
    remaining-=uint32_t(chunk);
  }
  if (!ok) { esp_http_client_close(client);esp_http_client_cleanup(client);return -1; }
  char response[256]{};
  return httpFinish(client,response,sizeof(response));
}

static bool wifiStatusRequest(const WifiJob& job,uint32_t& nextSegment,bool& finalized,int& status) {
  char url[320],response[320]{};
  snprintf(url,sizeof(url),"%s/v1/device-uploads/%s/status",job.endpoint,job.recordingId);
  status=httpJson(url,HTTP_METHOD_GET,job.token,nullptr,response,sizeof(response));
  if (status<200 || status>=300) return false;
  return jsonUInt(response,"next_segment",nextSegment) && jsonBool(response,"finalized",finalized);
}

static void wifiUploadTask(void*) {
  WifiJob job{};
  portENTER_CRITICAL(&wifiStatusMux);
  snprintf(job.endpoint,sizeof(job.endpoint),"%s",wifiJob.endpoint);
  snprintf(job.token,sizeof(job.token),"%s",wifiJob.token);
  snprintf(job.recordingId,sizeof(job.recordingId),"%s",wifiJob.recordingId);
  snprintf(job.logicalPath,sizeof(job.logicalPath),"%s",wifiJob.logicalPath);
  portEXIT_CRITICAL(&wifiStatusMux);

  bool storageFault=false,success=false;
  int httpStatus=0;
  char ssid[33]{},password[64]{};
  wifiSetStatus(1,"Connecting to Wi-Fi");
  if (!wifiLoadProfile(ssid,sizeof(ssid),password,sizeof(password))) {
    wifiSetStatus(6,"Wi-Fi is not configured",0,1);goto finish;
  }
  WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.begin(ssid,password);
  {
    const uint32_t started=millis();
    while (WiFi.status()!=WL_CONNECTED && uint32_t(millis()-started)<25000u) vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status()!=WL_CONNECTED) { wifiSetStatus(6,"Could not join Wi-Fi",0,2);goto finish; }

  wifiSetStatus(2,"Checking upload resume point");
  {
    OdysseySdGuard guard(pdMS_TO_TICKS(5000));
    if (!guard || !storageReady()) { wifiSetStatus(6,"SD card unavailable",0,3);goto disconnect; }
    char full[96]{};
    if (!fullPath(job.logicalPath,full,sizeof(full))) { wifiSetStatus(6,"Invalid SD recording path",0,4);goto disconnect; }
    struct stat st{};
    if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=44 || uint64_t(st.st_size)>UINT32_MAX) {
      wifiSetStatus(6,"SD recording unavailable",0,5);goto disconnect;
    }
    FILE* file=fopen(full,"rb");
    if (!file) { wifiSetStatus(6,"Could not open SD recording",0,6);goto disconnect; }
    uint8_t sourceHeader[44]{};
    const bool headerOk=fread(sourceHeader,1,sizeof(sourceHeader),file)==sizeof(sourceHeader) &&
      !memcmp(sourceHeader,"RIFF",4) && !memcmp(sourceHeader+8,"WAVE",4) &&
      !memcmp(sourceHeader+36,"data",4);
    const uint32_t dataBytes=headerOk ?
      uint32_t(sourceHeader[40]) | (uint32_t(sourceHeader[41])<<8) | (uint32_t(sourceHeader[42])<<16) | (uint32_t(sourceHeader[43])<<24) : 0;
    if (!headerOk || dataBytes!=uint32_t(st.st_size-44) || !dataBytes || (dataBytes&1u)) {
      fclose(file);wifiSetStatus(6,"SD WAV is incomplete",0,7);goto disconnect;
    }
    const uint32_t segmentCount=(dataBytes+WIFI_SEGMENT_PCM_BYTES-1u)/WIFI_SEGMENT_PCM_BYTES;
    const uint32_t durationMs=uint32_t((uint64_t(dataBytes)*1000u)/(SAMPLE_RATE*2u));
    portENTER_CRITICAL(&wifiStatusMux);
    wifiSegmentCount=segmentCount;wifiTotalBytes=uint32_t(st.st_size);wifiUploadedBytes=0;
    portEXIT_CRITICAL(&wifiStatusMux);

    uint32_t next=0;bool finalized=false;
    if (!wifiStatusRequest(job,next,finalized,httpStatus) || next>segmentCount) {
      fclose(file);wifiSetStatus(6,"Could not resume cloud upload",httpStatus,8);goto disconnect;
    }
    if (finalized) {
      if (unlink(full)!=0) { wifiSetStatus(6,"Cloud has recording but SD cleanup failed",httpStatus,9);goto disconnect; }
      fclose(file);success=true;wifiSetStatus(5,"Wi-Fi sync complete",httpStatus,0);goto disconnect;
    }
    wifiUploadedBytes=std::min(dataBytes,next*WIFI_SEGMENT_PCM_BYTES);
    for (uint32_t index=next;index<segmentCount;++index) {
      const uint32_t pcmOffset=index*WIFI_SEGMENT_PCM_BYTES;
      const uint32_t pcmBytes=std::min(WIFI_SEGMENT_PCM_BYTES,dataBytes-pcmOffset);
      const uint32_t startMs=uint32_t((uint64_t(pcmOffset)*1000u)/(SAMPLE_RATE*2u));
      const uint32_t endMs=uint32_t((uint64_t(pcmOffset+pcmBytes)*1000u)/(SAMPLE_RATE*2u));
      portENTER_CRITICAL(&wifiStatusMux);wifiPhase=3;wifiSegment=index;portEXIT_CRITICAL(&wifiStatusMux);
      httpStatus=uploadWavSegment(file,job,index,pcmOffset,pcmBytes,startMs,endMs);
      if (httpStatus<200 || httpStatus>=300) {
        fclose(file);wifiSetStatus(6,"Wi-Fi segment upload failed",httpStatus,10);goto disconnect;
      }
      wifiUploadedBytes=pcmOffset+pcmBytes;
    }
    fclose(file);

    wifiSetStatus(4,"Finalizing cloud recording");
    char url[320],body[128],response[256]{};
    snprintf(url,sizeof(url),"%s/v1/device-uploads/%s/finalize",job.endpoint,job.recordingId);
    snprintf(body,sizeof(body),"{\"duration_ms\":%lu,\"segment_count\":%lu}",
      static_cast<unsigned long>(durationMs),static_cast<unsigned long>(segmentCount));
    httpStatus=httpJson(url,HTTP_METHOD_POST,job.token,body,response,sizeof(response));
    if (httpStatus<200 || httpStatus>=300) { wifiSetStatus(6,"Cloud finalize failed",httpStatus,11);goto disconnect; }
    next=0;finalized=false;
    if (!wifiStatusRequest(job,next,finalized,httpStatus) || !finalized || next<segmentCount) {
      wifiSetStatus(6,"Cloud verification incomplete",httpStatus,12);goto disconnect;
    }
    if (unlink(full)!=0) { wifiSetStatus(6,"Verified upload; SD cleanup failed",httpStatus,13);goto disconnect; }
    success=true;wifiUploadedBytes=dataBytes;wifiSetStatus(5,"Wi-Fi sync complete",httpStatus,0);
  }

disconnect:
  WiFi.disconnect(true,false);WiFi.mode(WIFI_OFF);
finish:
  if (!success && storageFault) {
    odysseySdMarkVfsFailure();
    (void)odysseySdQuiesceFaultedSession(750u);
  }
  memset(&wifiJob,0,sizeof(wifiJob));
  wifiUploadActive=false;
  vTaskDelete(nullptr);
}

static uint8_t wifiStatusReply(uint8_t* bytes,size_t& size) {
  char ssid[33]{},password[64]{};
  const bool configured=wifiProfileConfigured.load() || wifiLoadProfile(ssid,sizeof(ssid),password,sizeof(password));
  if (configured) wifiProfileConfigured=true;
  uint8_t phase;uint32_t segment,count,uploaded,total;int http,error;char message[72],recording[40];
  portENTER_CRITICAL(&wifiStatusMux);
  phase=wifiPhase;segment=wifiSegment;count=wifiSegmentCount;uploaded=wifiUploadedBytes;total=wifiTotalBytes;
  http=wifiHttpStatus;error=wifiErrorCode;
  snprintf(message,sizeof(message),"%s",wifiStatusMessage);
  snprintf(recording,sizeof(recording),"%s",wifiRecordingId);
  portEXIT_CRITICAL(&wifiStatusMux);
  const int n=snprintf(reinterpret_cast<char*>(bytes),480,
    "{\"configured\":%s,\"active\":%s,\"phase\":%u,\"segment\":%lu,\"segments\":%lu,\"uploaded\":%lu,\"total\":%lu,\"http\":%d,\"error\":%d,\"recording\":\"%s\",\"message\":\"%s\"}",
    configured?"true":"false",wifiUploadActive.load()?"true":"false",unsigned(phase),
    static_cast<unsigned long>(segment),static_cast<unsigned long>(count),
    static_cast<unsigned long>(uploaded),static_cast<unsigned long>(total),http,error,recording,message);
  if (n<=0) return IO_ERROR;
  size=std::min(size_t(n),size_t(479));return OK;
}

static uint8_t wifiCommit(uint32_t mode) {
  if (wifiUploadActive.load()) return BUSY;
  if (mode==1) {
    char ssid[33]{},password[64]{};
    if (!stageField('S',ssid,sizeof(ssid)) || !stageField('P',password,sizeof(password),false) ||
        !wifiSaveProfile(ssid,password)) return BAD_COMMAND;
    wifiSetStatus(0,"Wi-Fi saved");
    memset(wifiStage,0,sizeof(wifiStage));wifiStageLength=0;
    return OK;
  }
  if (mode!=2 || !wifiProfileConfigured.load()) {
    char s[33]{},p[64]{};
    if (!wifiLoadProfile(s,sizeof(s),p,sizeof(p))) return NO_SD;
    wifiProfileConfigured=true;
    if (mode!=2) return BAD_COMMAND;
  }
  WifiJob job{};
  if (!stageField('E',job.endpoint,sizeof(job.endpoint)) ||
      !stageField('T',job.token,sizeof(job.token)) ||
      !stageField('R',job.recordingId,sizeof(job.recordingId)) ||
      !stageField('F',job.logicalPath,sizeof(job.logicalPath)) ||
      !secureEndpoint(job.endpoint) || strlen(job.recordingId)!=36 || !safeWavPath(job.logicalPath)) return BAD_COMMAND;
  bool expected=false;
  if (!wifiUploadActive.compare_exchange_strong(expected,true)) return BUSY;
  portENTER_CRITICAL(&wifiStatusMux);
  wifiJob=job;
  snprintf(wifiRecordingId,sizeof(wifiRecordingId),"%s",job.recordingId);
  wifiPhase=1;wifiSegment=wifiSegmentCount=wifiUploadedBytes=wifiTotalBytes=0;wifiHttpStatus=wifiErrorCode=0;
  snprintf(wifiStatusMessage,sizeof(wifiStatusMessage),"%s","Starting Wi-Fi sync");
  portEXIT_CRITICAL(&wifiStatusMux);
  memset(wifiStage,0,sizeof(wifiStage));wifiStageLength=0;
  if (xTaskCreate(wifiUploadTask,"c3-wifi-sync",16384,nullptr,1,nullptr)!=pdPASS) {
    wifiUploadActive=false;memset(&wifiJob,0,sizeof(wifiJob));wifiSetStatus(6,"Wi-Fi task unavailable",0,14);return IO_ERROR;
  }
  return OK;
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
    if (wifiUploadActive.load() && request.operation!=25) { reply(request,BUSY);continue; }
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
      case 23: error=wifiStageChunk(request.offset,request.path,total); break;
      case 24: error=wifiCommit(request.offset); break;
      case 25: error=wifiStatusReply(bytes,size); break;
      case 26:
        if (wifiUploadActive.load()) error=BUSY;
        else { wifiForgetProfile();wifiSetStatus(0,"Wi-Fi forgotten");error=OK; }
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
bool wifiAvailable(){
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  return requests!=nullptr;
#else
  return false;
#endif
}

void initialize() {
  char ssid[33]{},password[64]{};
  wifiProfileConfigured=wifiLoadProfile(ssid,sizeof(ssid),password,sizeof(password));
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
