// Odyssey C3 Wi-Fi SD upload.
// Control remains on BLE. The ESP32-C3 joins a user-provisioned 2.4 GHz Wi-Fi
// network (home Wi-Fi or phone hotspot) only for an explicit upload session,
// then streams the append-only SD WAV directly to Synap Cloud over HTTPS.
// The SD source is never deleted here; retention stays a separate PWA choice.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <limits.h>
#include <time.h>

namespace OdysseyWifi {

// Google Trust Services GTS Root R1 (current digitalSignature root).
// SHA-256: D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:
//           80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
// Valid until 2036-06-22. Source: https://pki.goog/repository/
// Arduino-ESP32 3.3.5 has setCACert(), but no useBuiltinCACertBundle().
static const char GOOGLE_TRUST_ROOT_R1[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)PEM";

static constexpr uint32_t WIFI_CONNECT_MS=20000u;
static constexpr uint32_t TIME_SYNC_MS=12000u;
static constexpr time_t TLS_MIN_UNIX_TIME=1704067200; // 2024-01-01 UTC
static constexpr uint32_t HTTP_TIMEOUT_MS=30000u;
static constexpr uint32_t SEGMENT_MS=120000u;
static constexpr uint32_t PCM_BYTES_PER_MS=32u; // 16 kHz * mono * 16 bit / 1000.
static constexpr uint32_t SEGMENT_PCM_BYTES=SEGMENT_MS*PCM_BYTES_PER_MS;
static constexpr size_t CONFIG_CAPACITY=2304;
static constexpr size_t WIFI_TASK_STACK=12288;

enum Phase : uint8_t {
  IDLE=0,
  CONNECTING=1,
  PREPARING=2,
  UPLOADING=3,
  FINALIZING=4,
  COMPLETE=5,
  FAILED=6,
};

struct Status {
  bool active=false;
  bool configured=false;
  uint8_t phase=IDLE;
  uint32_t total=0;
  uint32_t uploaded=0;
  int16_t http=0;
  char ssid[33]{};
  char message[96]{};
};

struct UploadSpec {
  char endpoint[192]{};
  char token[896]{};
  char recordingId[40]{};
  char path[64]{};
};

static portMUX_TYPE statusMux=portMUX_INITIALIZER_UNLOCKED;
static Status statusState;
static UploadSpec uploadSpec;
static std::atomic<bool> uploadBusy{false};
static char configBuffer[CONFIG_CAPACITY]{};
static size_t configLength=0;

static void setStatus(uint8_t phase,bool active,uint32_t total,uint32_t uploaded,
                      int http,const char* message) {
  portENTER_CRITICAL(&statusMux);
  statusState.phase=phase;
  statusState.active=active;
  statusState.total=total;
  statusState.uploaded=uploaded;
  statusState.http=int16_t(http);
  if (message) snprintf(statusState.message,sizeof(statusState.message),"%s",message);
  portEXIT_CRITICAL(&statusMux);
}

static bool readCredentials(char* ssid,size_t ssidCap,char* password,size_t passwordCap) {
  if (!ssid || ssidCap<2 || !password || passwordCap<1) return false;
  ssid[0]=0;password[0]=0;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",true)) return false;
  const String storedSsid=prefs.getString("ssid","");
  const String storedPassword=prefs.getString("pass","");
  prefs.end();
  if (!storedSsid.length() || storedSsid.length()>32 || storedPassword.length()>63) return false;
  snprintf(ssid,ssidCap,"%s",storedSsid.c_str());
  snprintf(password,passwordCap,"%s",storedPassword.c_str());
  return true;
}

static void refreshConfigured() {
  char ssid[33],password[64];
  const bool ok=readCredentials(ssid,sizeof(ssid),password,sizeof(password));
  portENTER_CRITICAL(&statusMux);
  statusState.configured=ok;
  snprintf(statusState.ssid,sizeof(statusState.ssid),"%s",ok?ssid:"");
  portEXIT_CRITICAL(&statusMux);
}

bool available() { return true; }
bool busy() { return uploadBusy.load(); }

static bool safeWavPath(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12 || n>63 || (strcasecmp(path+n-4,".wav")!=0)) return false;
  for (size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}

static int hexNibble(char c) {
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return c-'a'+10;
  if (c>='A'&&c<='F') return c-'A'+10;
  return -1;
}

static bool decodeField(const char* key,char* output,size_t capacity) {
  if (!key || !output || capacity<1) return false;
  const size_t keyLength=strlen(key);
  const char* start=configBuffer;
  const char* limit=configBuffer+configLength;
  while (start<limit) {
    const char* end=static_cast<const char*>(memchr(start,'\n',size_t(limit-start)));
    if (!end) end=limit;
    if (size_t(end-start)>=keyLength && !memcmp(start,key,keyLength)) {
      const char* hex=start+keyLength;
      const size_t digits=size_t(end-hex);
      if (digits&1u || digits/2u>=capacity) return false;
      const size_t bytes=digits/2u;
      for (size_t i=0;i<bytes;++i) {
        const int hi=hexNibble(hex[i*2]),lo=hexNibble(hex[i*2+1]);
        if (hi<0 || lo<0) return false;
        output[i]=char((hi<<4)|lo);
      }
      output[bytes]=0;
      return true;
    }
    start=end<limit?end+1:limit;
  }
  return false;
}

uint32_t configChunk(uint32_t offset,const char* text) {
  const size_t n=text?strlen(text):0;
  if (offset==0) { configLength=0;configBuffer[0]=0; }
  if (offset!=configLength || !n || n>60 || configLength+n>=sizeof(configBuffer)) return UINT32_MAX;
  memcpy(configBuffer+configLength,text,n);
  configLength+=n;
  configBuffer[configLength]=0;
  return uint32_t(configLength);
}

static bool saveCredentialsFromConfig() {
  char ssid[33]{},password[64]{};
  if (!decodeField("S=",ssid,sizeof(ssid)) || !ssid[0]) return false;
  // An open network is valid, so a missing P= is treated as empty only when
  // the field itself exists in the configuration.
  if (!decodeField("P=",password,sizeof(password))) return false;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",false)) return false;
  const size_t a=prefs.putString("ssid",ssid);
  const size_t b=prefs.putString("pass",password);
  prefs.end();
  if (!a || (password[0] && !b)) return false;
  refreshConfigured();
  return true;
}

bool forget() {
  if (busy()) return false;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",false)) return false;
  const bool ok=prefs.clear();
  prefs.end();
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
  refreshConfigured();
  setStatus(IDLE,false,0,0,0,ok?"Wi-Fi network removed.":"Could not remove Wi-Fi network.");
  return ok;
}

static void wavHeader(uint8_t* h,uint32_t pcmBytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16u);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,pcmBytes);
}

class SegmentStream : public Stream {
 public:
  SegmentStream(FILE* file,uint32_t pcmOffset,uint32_t pcmBytes)
    : file_(file),pcmBytes_(pcmBytes),remaining_(pcmBytes+44u),position_(0) {
    wavHeader(header_,pcmBytes_);
    if (file_) {
      if (fseek(file_,long(44u+pcmOffset),SEEK_SET)!=0) failed_=true;
    } else failed_=true;
    setTimeout(5000);
  }
  int available() override {
    if (failed_ || !remaining_) return 0;
    return remaining_>uint32_t(INT_MAX)?INT_MAX:int(remaining_);
  }
  int read() override {
    uint8_t byte=0;
    return readBytes(&byte,1)==1?int(byte):-1;
  }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }
  size_t readBytes(char* buffer,size_t length) override {
    return readBytes(reinterpret_cast<uint8_t*>(buffer),length);
  }
  size_t readBytes(uint8_t* buffer,size_t length) override {
    if (!buffer || !length || failed_ || !remaining_) return 0;
    size_t wanted=length;
    if (wanted>remaining_) wanted=remaining_;
    size_t done=0;
    if (position_<44u && wanted) {
      const size_t headerRemaining=44u-position_;
      const size_t take=wanted<headerRemaining?wanted:headerRemaining;
      memcpy(buffer,header_+position_,take);
      buffer+=take;wanted-=take;done+=take;position_+=take;remaining_-=take;
    }
    if (wanted) {
      const size_t got=fread(buffer,1,wanted,file_);
      done+=got;position_+=got;remaining_-=got;
      if (got!=wanted) failed_=true;
    }
    return done;
  }
  bool good() const { return !failed_ && remaining_==0; }
 private:
  FILE* file_=nullptr;
  uint32_t pcmBytes_=0,remaining_=0,position_=0;
  uint8_t header_[44]{};
  bool failed_=false;
};

static bool httpBegin(HTTPClient& http,WiFiClientSecure& tls,const String& url) {
  tls.setCACert(GOOGLE_TRUST_ROOT_R1);
  tls.setHandshakeTimeout(15);
  http.setConnectTimeout(15000);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  return http.begin(tls,url);
}

static void bearer(HTTPClient& http) {
  http.addHeader("Authorization","SynapDevice "+String(uploadSpec.token));
  http.addHeader("Cache-Control","no-store");
}

static bool uploadSegment(FILE* file,uint32_t index,uint32_t pcmOffset,uint32_t pcmBytes,int& httpCode) {
  WiFiClientSecure tls;
  HTTPClient http;
  const String url=String(uploadSpec.endpoint)+"/v1/device-uploads/"+uploadSpec.recordingId+
    "/segments/"+String(index);
  if (!httpBegin(http,tls,url)) { httpCode=-1;return false; }
  bearer(http);
  http.addHeader("Content-Type","audio/wav");
  http.addHeader("x-synap-start-ms",String(pcmOffset/PCM_BYTES_PER_MS));
  http.addHeader("x-synap-end-ms",String((pcmOffset+pcmBytes)/PCM_BYTES_PER_MS));
  SegmentStream stream(file,pcmOffset,pcmBytes);
  httpCode=http.sendRequest("PUT",&stream,size_t(pcmBytes)+44u);
  const bool ok=(httpCode==200 || httpCode==202) && stream.good();
  http.end();
  return ok;
}

static bool finalizeUpload(uint32_t durationMs,uint32_t segmentCount,int& httpCode) {
  WiFiClientSecure tls;
  HTTPClient http;
  const String url=String(uploadSpec.endpoint)+"/v1/device-uploads/"+uploadSpec.recordingId+"/finalize";
  if (!httpBegin(http,tls,url)) { httpCode=-1;return false; }
  bearer(http);
  http.addHeader("Content-Type","application/json");
  const String body=String("{\"duration_ms\":")+String(durationMs)+
    ",\"segment_count\":"+String(segmentCount)+"}";
  httpCode=http.POST(body);
  const bool ok=httpCode==200 || httpCode==202;
  http.end();
  return ok;
}

static bool connectWifi() {
  char ssid[33]{},password[64]{};
  if (!readCredentials(ssid,sizeof(ssid),password,sizeof(password))) return false;
  setStatus(CONNECTING,true,0,0,0,"Connecting Odyssey C3 to Wi-Fi…");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid,password);
  const uint32_t started=millis();
  while (WiFi.status()!=WL_CONNECTED && uint32_t(millis()-started)<WIFI_CONNECT_MS) {
    if (otaBusy() || sleepPending) break;
    delay(100);
  }
  if (WiFi.status()!=WL_CONNECTED) {
    WiFi.disconnect(true,false);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  return true;
}

static bool syncClock() {
  if (time(nullptr)>=TLS_MIN_UNIX_TIME) return true;
  setStatus(CONNECTING,true,0,0,0,"Securing Wi-Fi connection…");
  configTime(0,0,"time.google.com","time.cloudflare.com","pool.ntp.org");
  const uint32_t started=millis();
  while (time(nullptr)<TLS_MIN_UNIX_TIME && uint32_t(millis()-started)<TIME_SYNC_MS) {
    if (otaBusy() || sleepPending || batteryCritical() || WiFi.status()!=WL_CONNECTED) return false;
    delay(100);
  }
  return time(nullptr)>=TLS_MIN_UNIX_TIME;
}

static void stopWifi() {
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
}

static void uploadTask(void*) {
  applyCpuPowerProfile(true);
  bool ok=false;
  int httpCode=0;
  uint32_t pcmTotal=0,uploaded=0,segments=0;
  char full[96]{};
  FILE* file=nullptr;

  setStatus(PREPARING,true,0,0,0,"Preparing SD recording for Wi-Fi sync…");

  if (!connectWifi()) {
    setStatus(FAILED,false,0,0,0,"Could not join the saved Wi-Fi/hotspot.");
    uploadBusy=false;
    applyCpuPowerProfile(false);
    vTaskDelete(nullptr);
    return;
  }
  if (!syncClock()) {
    stopWifi();
    setStatus(FAILED,false,0,0,0,"Wi-Fi connected, but secure time sync failed.");
    uploadBusy=false;
    applyCpuPowerProfile(false);
    vTaskDelete(nullptr);
    return;
  }

  {
    // Keep storage mounted and the file stable for the complete cloud upload.
    OdysseySdGuard storage(pdMS_TO_TICKS(5000));
    if (storage && odysseySdReady() && safeWavPath(uploadSpec.path) &&
        odysseySdPath(uploadSpec.path,full,sizeof(full))) {
      struct stat st{};
      if (stat(full,&st)==0 && S_ISREG(st.st_mode) && st.st_size>44 &&
          uint64_t(st.st_size)<=uint64_t(LONG_MAX)) {
        pcmTotal=uint32_t(st.st_size)-44u;
        file=fopen(full,"rb");
      }
    }

    if (!file || !pcmTotal) {
      if (file) fclose(file);
      setStatus(FAILED,false,0,0,0,"SD recording is unavailable.");
    } else {
      setStatus(UPLOADING,true,pcmTotal,0,0,"Uploading SD recording over Wi-Fi…");
      const uint32_t segmentCount=(pcmTotal+SEGMENT_PCM_BYTES-1u)/SEGMENT_PCM_BYTES;
      for (uint32_t index=0;index<segmentCount;++index) {
        if (otaBusy() || sleepPending || batteryCritical() || WiFi.status()!=WL_CONNECTED) break;
        const uint32_t offset=index*SEGMENT_PCM_BYTES;
        const uint32_t remaining=pcmTotal-offset;
        const uint32_t bytes=remaining<SEGMENT_PCM_BYTES?remaining:SEGMENT_PCM_BYTES;
        if (!uploadSegment(file,index,offset,bytes,httpCode)) break;
        uploaded+=bytes;
        segments=index+1u;
        setStatus(UPLOADING,true,pcmTotal,uploaded,httpCode,"Uploading SD recording over Wi-Fi…");
      }
      if (uploaded==pcmTotal && segments==segmentCount) {
        setStatus(FINALIZING,true,pcmTotal,uploaded,httpCode,"Verifying Wi-Fi upload…");
        const uint32_t durationMs=pcmTotal/PCM_BYTES_PER_MS;
        ok=finalizeUpload(durationMs,segmentCount,httpCode);
      }
      fclose(file);file=nullptr;
      if (ok) {
        setStatus(COMPLETE,false,pcmTotal,uploaded,httpCode,"Wi-Fi sync complete.");
      } else {
        setStatus(FAILED,false,pcmTotal,uploaded,httpCode,
          WiFi.status()==WL_CONNECTED?"Wi-Fi upload failed; SD original kept.":"Wi-Fi disconnected; SD original kept.");
      }
    }
  }

  stopWifi();
  uploadBusy=false;
  applyCpuPowerProfile(false);
  vTaskDelete(nullptr);
}

static bool prepareUploadFromConfig() {
  UploadSpec next{};
  if (!decodeField("E=",next.endpoint,sizeof(next.endpoint)) ||
      !decodeField("T=",next.token,sizeof(next.token)) ||
      !decodeField("R=",next.recordingId,sizeof(next.recordingId)) ||
      !decodeField("F=",next.path,sizeof(next.path))) return false;
  if (strncmp(next.endpoint,"https://",8)!=0 || strchr(next.endpoint+8,'/') ||
      strlen(next.token)<16 || strlen(next.recordingId)!=36 || !safeWavPath(next.path)) return false;
  if (busy() || odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending ||
      !odysseySdReady()) return false;
  uploadSpec=next;
  uploadBusy=true;
  setStatus(PREPARING,true,0,0,0,"Starting Wi-Fi sync…");
  if (xTaskCreate(uploadTask,"c3-wifi",WIFI_TASK_STACK,nullptr,1,nullptr)!=pdPASS) {
    uploadBusy=false;
    setStatus(FAILED,false,0,0,0,"Wi-Fi upload task unavailable.");
    return false;
  }
  return true;
}

bool applyConfig(uint32_t mode) {
  bool ok=false;
  if (mode==1) ok=saveCredentialsFromConfig();
  else if (mode==2) ok=prepareUploadFromConfig();
  configLength=0;configBuffer[0]=0;
  if (!ok && mode==1) setStatus(FAILED,false,0,0,0,"Wi-Fi network could not be saved.");
  return ok;
}

size_t encode(char* output,size_t capacity) {
  if (!output || capacity<32) return 0;
  refreshConfigured();
  Status snapshot;
  portENTER_CRITICAL(&statusMux);
  snapshot=statusState;
  portEXIT_CRITICAL(&statusMux);
  const int n=snprintf(output,capacity,
    "{\"configured\":%s,\"active\":%s,\"phase\":%u,\"total\":%lu,\"uploaded\":%lu,"
    "\"http\":%d,\"message\":\"%s\"}",
    snapshot.configured?"true":"false",snapshot.active?"true":"false",unsigned(snapshot.phase),
    static_cast<unsigned long>(snapshot.total),static_cast<unsigned long>(snapshot.uploaded),
    int(snapshot.http),snapshot.message);
  if (n<0) return 0;
  return size_t(n)<capacity?size_t(n):capacity-1;
}

void initialize() {
  refreshConfigured();
  setStatus(IDLE,false,0,0,0,"Wi-Fi idle.");
}

} // namespace OdysseyWifi
#endif
