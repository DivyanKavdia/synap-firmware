// Odyssey C3 SD media: verified BLE transfer plus direct Wi-Fi cloud upload.
// C3 storage is mounted through the stock Arduino SD SPI path and accessed through FAT/VFS.
// Files are deleted only after the destination has durably accepted and verified them.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_tls.h>
#include <esp_tls_errors.h>
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
static std::atomic<bool> nativeWifiGotIp{false},nativeWifiDisconnected{false};
static esp_netif_t* nativeWifiNetif=nullptr;
static bool nativeWifiInitialized=false;

static void nativeWifiEvent(void*,esp_event_base_t base,int32_t id,void*) {
  if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
    nativeWifiGotIp=true;nativeWifiDisconnected=false;
  } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
    nativeWifiGotIp=false;nativeWifiDisconnected=true;
  }
}

static bool nativeWifiInitialize() {
  if (nativeWifiInitialized) return true;
  const esp_err_t netifInit=esp_netif_init();
  if (netifInit!=ESP_OK && netifInit!=ESP_ERR_INVALID_STATE) return false;
  const esp_err_t loopInit=esp_event_loop_create_default();
  if (loopInit!=ESP_OK && loopInit!=ESP_ERR_INVALID_STATE) return false;
  nativeWifiNetif=esp_netif_create_default_wifi_sta();
  if (!nativeWifiNetif) return false;
  wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg)!=ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&nativeWifiEvent,nullptr)!=ESP_OK ||
      esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&nativeWifiEvent,nullptr)!=ESP_OK) return false;
  nativeWifiInitialized=true;
  return true;
}

static bool nativeWifiConnect(const char* ssid,const char* password,uint32_t timeoutMs) {
  if (!ssid || !ssid[0] || !nativeWifiInitialize()) return false;
  wifi_config_t cfg={};
  snprintf(reinterpret_cast<char*>(cfg.sta.ssid),sizeof(cfg.sta.ssid),"%s",ssid);
  snprintf(reinterpret_cast<char*>(cfg.sta.password),sizeof(cfg.sta.password),"%s",password?password:"");
  cfg.sta.pmf_cfg.capable=true;cfg.sta.pmf_cfg.required=false;
  if (esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK ||
      esp_wifi_set_config(WIFI_IF_STA,&cfg)!=ESP_OK) return false;
  const esp_err_t start=esp_wifi_start();
  if (start!=ESP_OK && start!=ESP_ERR_WIFI_CONN) return false;
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  nativeWifiGotIp=false;nativeWifiDisconnected=false;
  if (esp_wifi_connect()!=ESP_OK) return false;
  const uint32_t started=millis();
  while (!nativeWifiGotIp.load() && uint32_t(millis()-started)<timeoutMs) {
    if (nativeWifiDisconnected.exchange(false)) (void)esp_wifi_connect();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return nativeWifiGotIp.load();
}

static void nativeWifiStop() {
  nativeWifiGotIp=false;nativeWifiDisconnected=false;
  if (!nativeWifiInitialized) return;
  (void)esp_wifi_disconnect();
  (void)esp_wifi_stop();
}

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

static const char SYNAP_GTS_ROOTS[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlrsWNBCUaqxElqjANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjIwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjIwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQDO3v2m++zsFDQ8BwZabFn3GTXd98GdVarTzTukk3LvCvpt\n"
"nfbwhYBboUhSnznFt+4orO/LdmgUud+tAWyZH8QiHZ/+cnfgLFuv5AS/T3KgGjSY\n"
"6Dlo7JUle3ah5mm5hRm9iYz+re026nO8/4Piy33B0s5Ks40FnotJk9/BW9BuXvAu\n"
"MC6C/Pq8tBcKSOWIm8Wba96wyrQD8Nr0kLhlZPdcTK3ofmZemde4wj7I0BOdre7k\n"
"RXuJVfeKH2JShBKzwkCX44ofR5GmdFrS+LFjKBC4swm4VndAoiaYecb+3yXuPuWg\n"
"f9RhD1FLPD+M2uFwdNjCaKH5wQzpoeJ/u1U8dgbuak7MkogwTZq9TwtImoS1mKPV\n"
"+3PBV2HdKFZ1E66HjucMUQkQdYhMvI35ezzUIkgfKtzra7tEscszcTJGr61K8Yzo\n"
"dDqs5xoic4DSMPclQsciOzsSrZYuxsN2B6ogtzVJV+mSSeh2FnIxZyuWfoqjx5RW\n"
"Ir9qS34BIbIjMt/kmkRtWVtd9QCgHJvGeJeNkP+byKq0rxFROV7Z+2et1VsRnTKa\n"
"G73VululycslaVNVJ1zgyjbLiGH7HrfQy+4W+9OmTN6SpdTi3/UGVN4unUu0kzCq\n"
"gc7dGtxRcw1PcOnlthYhGXmy5okLdWTK1au8CcEYof/UVKGFPP0UJAOyh9OktwID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQUu//KjiOfT5nK2+JopqUVJxce2Q4wDQYJKoZIhvcNAQEMBQADggIBAB/Kzt3H\n"
"vqGf2SdMC9wXmBFqiN495nFWcrKeGk6c1SuYJF2ba3uwM4IJvd8lRuqYnrYb/oM8\n"
"0mJhwQTtzuDFycgTE1XnqGOtjHsB/ncw4c5omwX4Eu55MaBBRTUoCnGkJE+M3DyC\n"
"B19m3H0Q/gxhswWV7uGugQ+o+MePTagjAiZrHYNSVc61LwDKgEDg4XSsYPWHgJ2u\n"
"NmSRXbBoGOqKYcl3qJfEycel/FVL8/B/uWU9J2jQzGv6U53hkRrJXRqWbTKH7QMg\n"
"yALOWr7Z6v2yTcQvG99fevX4i8buMTolUVVnjWQye+mew4K6Ki3pHrTgSAai/Gev\n"
"HyICc/sgCq+dVEuhzf9gR7A/Xe8bVr2XIZYtCtFenTgCR2y59PYjJbigapordwj6\n"
"xLEokCZYCDzifqrXPW+6MYgKBesntaFJ7qBFVHvmJ2WZICGoo7z7GJa7Um8M7YNR\n"
"TOlZ4iBgxcJlkoKM8xAfDoqXvneCbT+PHV28SSe9zE8P4c52hgQjxcCMElv924Sg\n"
"JPFI/2R80L5cFtHvma3AH/vLrrw4IgYmZNralw4/KBVEqE8AyvCazM90arQ+POuV\n"
"7LXTWtiBmelDGDfrs7vRWGJB82bSj6p4lVQgw1oudCvV0b4YacCs1aTPObpRhANl\n"
"6WLAYv7YTVWW4tAR+kg0Eeye7QUd5MjWHYbL\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G\n"
"jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2\n"
"4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7\n"
"VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm\n"
"ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
"QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
"HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
"9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
"p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
"-----END CERTIFICATE-----\n";

struct HttpsTarget { char host[128]{};char base[64]{}; };

static bool parseHttpsEndpoint(const char* endpoint,HttpsTarget& target) {
  if (!endpoint || strncmp(endpoint,"https://",8)!=0) return false;
  const char* host=endpoint+8;
  const char* slash=strchr(host,'/');
  const size_t hostLength=slash?size_t(slash-host):strlen(host);
  if (!hostLength || hostLength>=sizeof(target.host)) return false;
  for (size_t i=0;i<hostLength;++i) {
    const char c=host[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-')) return false;
  }
  memcpy(target.host,host,hostLength);target.host[hostLength]=0;
  if (slash) {
    const size_t baseLength=strlen(slash);
    if (baseLength>=sizeof(target.base) || strchr(slash,'?') || strchr(slash,'#')) return false;
    snprintf(target.base,sizeof(target.base),"%s",slash);
    while (strlen(target.base)>1 && target.base[strlen(target.base)-1]=='/') target.base[strlen(target.base)-1]=0;
  }
  return true;
}

static bool secureEndpoint(const char* endpoint) {
  HttpsTarget target;
  return parseHttpsEndpoint(endpoint,target);
}

static bool tlsRetry(ssize_t result) {
  return result==ESP_TLS_ERR_SSL_WANT_READ || result==ESP_TLS_ERR_SSL_WANT_WRITE;
}

static void closeHttps(esp_tls_t*& tls) {
  if (tls) { esp_tls_conn_destroy(tls);tls=nullptr; }
}

static bool netWriteAll(esp_tls_t* tls,const uint8_t* bytes,size_t size) {
  if (!tls) return false;
  size_t written=0;const uint32_t started=millis();
  while (written<size && uint32_t(millis()-started)<30000u) {
    const ssize_t n=esp_tls_conn_write(tls,bytes+written,size-written);
    if (n>0) { written+=size_t(n);continue; }
    if (!tlsRetry(n)) return false;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return written==size;
}

static int netReadByte(esp_tls_t* tls,uint32_t& last,uint32_t timeoutMs=30000) {
  uint8_t byte=0;
  while (uint32_t(millis()-last)<timeoutMs) {
    const ssize_t n=esp_tls_conn_read(tls,&byte,1);
    if (n==1) { last=millis();return int(byte); }
    if (n==0) return -2;
    if (!tlsRetry(n)) return -2;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return -1;
}

static bool netLine(esp_tls_t* tls,char* line,size_t capacity,uint32_t timeoutMs=30000) {
  if (!tls || !line || capacity<2) return false;
  size_t n=0;uint32_t last=millis();
  for (;;) {
    const int value=netReadByte(tls,last,timeoutMs);
    if (value<0) { line[n]=0;return false; }
    if (value=='\n') { line[n]=0;return true; }
    if (value!='\r' && n+1<capacity) line[n++]=char(value);
  }
}

static int netResponse(esp_tls_t*& tls,char* response,size_t capacity) {
  char line[192]{};
  if (!netLine(tls,line,sizeof(line)) || strncmp(line,"HTTP/1.",7)!=0) { closeHttps(tls);return -1; }
  const char* code=strchr(line,' ');
  if (!code) { closeHttps(tls);return -1; }
  const int status=atoi(code+1);
  do {
    if (!netLine(tls,line,sizeof(line))) { closeHttps(tls);return -1; }
  } while (line[0]);
  if (response && capacity) {
    size_t n=0;uint32_t last=millis();
    for (;;) {
      const int value=netReadByte(tls,last,30000);
      if (value==-2) break;
      if (value<0) { closeHttps(tls);return -1; }
      if (n+1<capacity) response[n++]=char(value);
    }
    response[n]=0;
  }
  closeHttps(tls);return status;
}

static esp_tls_t* openHttps(const char* endpoint,HttpsTarget& target) {
  if (!parseHttpsEndpoint(endpoint,target)) return nullptr;
  esp_tls_t* tls=esp_tls_init();
  if (!tls) return nullptr;
  esp_tls_cfg_t cfg={};
  cfg.cacert_buf=reinterpret_cast<const unsigned char*>(SYNAP_GTS_ROOTS);
  cfg.cacert_bytes=sizeof(SYNAP_GTS_ROOTS);
  cfg.common_name=target.host;
  cfg.timeout_ms=30000;
  if (esp_tls_conn_new_sync(target.host,int(strlen(target.host)),443,&cfg,tls)!=1) {
    closeHttps(tls);return nullptr;
  }
  return tls;
}

static int httpJson(const char* endpoint,const char* method,const char* token,
                    const char* path,const char* body,char* response,size_t capacity) {
  HttpsTarget target;esp_tls_t* client=openHttps(endpoint,target);
  if (!client) return -1;
  char fullPath[384],header[1536];
  const int p=snprintf(fullPath,sizeof(fullPath),"%s%s",target.base,path);
  const size_t bytes=body?strlen(body):0;
  const int h=snprintf(header,sizeof(header),
    "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: SynapDevice %s\r\n"
    "User-Agent: Synap-Odyssey-C3/1\r\nContent-Type: application/json\r\n"
    "Content-Length: %u\r\nConnection: close\r\n\r\n",
    method,fullPath,target.host,token?token:"",unsigned(bytes));
  if (p<=0 || size_t(p)>=sizeof(fullPath) || h<=0 || size_t(h)>=sizeof(header) ||
      !netWriteAll(client,reinterpret_cast<const uint8_t*>(header),size_t(h)) ||
      (bytes && !netWriteAll(client,reinterpret_cast<const uint8_t*>(body),bytes))) {
    closeHttps(client);return -1;
  }
  return netResponse(client,response,capacity);
}

static int uploadWavSegment(FILE* file,const WifiJob& job,uint32_t index,uint32_t pcmOffset,
                            uint32_t pcmBytes,uint32_t startMs,uint32_t endMs) {
  HttpsTarget target;esp_tls_t* client=openHttps(job.endpoint,target);
  if (!client) return -1;
  char path[384],header[1536];
  const int p=snprintf(path,sizeof(path),"%s/v1/device-uploads/%s/segments/%lu",
    target.base,job.recordingId,static_cast<unsigned long>(index));
  const uint32_t bodyBytes=44u+pcmBytes;
  const int h=snprintf(header,sizeof(header),
    "PUT %s HTTP/1.1\r\nHost: %s\r\nAuthorization: SynapDevice %s\r\n"
    "User-Agent: Synap-Odyssey-C3/1\r\nContent-Type: audio/wav\r\n"
    "Content-Length: %lu\r\nX-Synap-Start-Ms: %lu\r\nX-Synap-End-Ms: %lu\r\n"
    "Connection: close\r\n\r\n",
    path,target.host,job.token,static_cast<unsigned long>(bodyBytes),
    static_cast<unsigned long>(startMs),static_cast<unsigned long>(endMs));
  if (p<=0 || size_t(p)>=sizeof(path) || h<=0 || size_t(h)>=sizeof(header) ||
      !netWriteAll(client,reinterpret_cast<const uint8_t*>(header),size_t(h))) {
    closeHttps(client);return -1;
  }
  uint8_t wav[44];::odysseyWavHeader(wav,pcmBytes);
  bool ok=netWriteAll(client,wav,sizeof(wav));
  if (ok && fseek(file,long(44u+pcmOffset),SEEK_SET)!=0) ok=false;
  uint8_t buffer[2048];
  uint32_t remaining=pcmBytes;
  while (ok && remaining) {
    const size_t chunk=std::min(size_t(remaining),sizeof(buffer));
    if (fread(buffer,1,chunk,file)!=chunk || !netWriteAll(client,buffer,chunk)) { ok=false;break; }
    remaining-=uint32_t(chunk);
  }
  if (!ok) { closeHttps(client);return -1; }
  return netResponse(client,nullptr,0);
}

static bool wifiStatusRequest(const WifiJob& job,uint32_t& nextSegment,bool& finalized,int& status) {
  char path[128],response[320]{};
  snprintf(path,sizeof(path),"/v1/device-uploads/%s/status",job.recordingId);
  status=httpJson(job.endpoint,"GET",job.token,path,nullptr,response,sizeof(response));
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
  if (!nativeWifiConnect(ssid,password,25000u)) {
    wifiSetStatus(6,"Could not join Wi-Fi",0,2);goto finish;
  }

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
      if (fclose(file)!=0) { wifiSetStatus(6,"Cloud has recording but SD close failed",httpStatus,9);goto disconnect; }
      file=nullptr;
      if (unlink(full)!=0) { wifiSetStatus(6,"Cloud has recording but SD cleanup failed",httpStatus,9);goto disconnect; }
      success=true;wifiSetStatus(5,"Wi-Fi sync complete",httpStatus,0);goto disconnect;
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
    char finalizePath[128],body[128],response[256]{};
    snprintf(finalizePath,sizeof(finalizePath),"/v1/device-uploads/%s/finalize",job.recordingId);
    snprintf(body,sizeof(body),"{\"duration_ms\":%lu,\"segment_count\":%lu}",
      static_cast<unsigned long>(durationMs),static_cast<unsigned long>(segmentCount));
    httpStatus=httpJson(job.endpoint,"POST",job.token,finalizePath,body,response,sizeof(response));
    if (httpStatus<200 || httpStatus>=300) { wifiSetStatus(6,"Cloud finalize failed",httpStatus,11);goto disconnect; }
    next=0;finalized=false;
    if (!wifiStatusRequest(job,next,finalized,httpStatus) || !finalized || next<segmentCount) {
      wifiSetStatus(6,"Cloud verification incomplete",httpStatus,12);goto disconnect;
    }
    if (unlink(full)!=0) { wifiSetStatus(6,"Verified upload; SD cleanup failed",httpStatus,13);goto disconnect; }
    success=true;wifiUploadedBytes=dataBytes;wifiSetStatus(5,"Wi-Fi sync complete",httpStatus,0);
  }

disconnect:
  nativeWifiStop();
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
bool wifiAvailable(){return requests!=nullptr;}
bool wifiBusy(){return wifiUploadActive.load();}

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
