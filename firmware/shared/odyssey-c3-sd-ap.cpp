// Odyssey C3+SD private read-only Wi-Fi hotspot.
// BLE operations 20/21/22 start, inspect and stop a temporary AP session.
// This intentionally never writes, deletes, remounts, or formats the SD.
// The separate cloud STA uploader remains available for compatibility.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#if SYNAP_C3_WIFI_UPLOAD_ENABLED
#include <WebServer.h>
#include <WiFi.h>
#include <dirent.h>
#include <sys/stat.h>
#include <new>
#include "esp_system.h"
#endif
namespace OdysseyWifiAp {
#if SYNAP_C3_WIFI_UPLOAD_ENABLED

static constexpr uint32_t IDLE_MS=180000u, MAX_MS=900000u;
static constexpr size_t AP_STACK=12288;
struct Info {
  bool active=false;
  char ssid[24]{};
  char password[33]{};
  char key[33]{};
};
static Info apInfo;
static portMUX_TYPE apMux=portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> apActive{false}, stopRequested{false};
static WebServer* apServer=nullptr;
static uint32_t startedAt=0,lastActivityAt=0;

static Info snapshot() {
  Info copy;
  portENTER_CRITICAL(&apMux); copy=apInfo; portEXIT_CRITICAL(&apMux);
  return copy;
}
static void store(const Info& next) {
  portENTER_CRITICAL(&apMux); apInfo=next; portEXIT_CRITICAL(&apMux);
}
bool busy() { return apActive.load(); }
static bool safeWav(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12u || n>63u || strcasecmp(path+n-4,".wav")!=0) return false;
  for(size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a' && c<='z')||(c>='A' && c<='Z')||
          (c>='0' && c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}
static bool authorized() {
  apServer->sendHeader("Cache-Control","no-store, no-cache, must-revalidate");
  apServer->sendHeader("Referrer-Policy","no-referrer");
  apServer->sendHeader("X-Content-Type-Options","nosniff");
  apServer->sendHeader("Content-Security-Policy",
    "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
  const Info current=snapshot();
  if (!current.active || apServer->arg("key")!=current.key) {
    apServer->send(403,"text/plain","Use the private link displayed in Synap.");
    return false;
  }
  lastActivityAt=millis();
  return true;
}
static void page() {
  if (!authorized()) return;
  const Info current=snapshot();
  apServer->setContentLength(CONTENT_LENGTH_UNKNOWN);
  apServer->send(200,"text/html","");
  apServer->sendContent("<!doctype html><html lang=en><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Synap C3 SD downloads</title><style>"
    "body{font:16px system-ui;background:#f6f8f6;color:#18352c;max-width:44rem;margin:auto;padding:22px}"
    "h1{font-size:25px}li{padding:12px 0;border-bottom:1px solid #d5dfd9;overflow-wrap:anywhere}"
    "a{color:#155f4a}button{font:inherit;padding:10px;border-radius:9px}"
    "</style><h1>Synap C3 recordings</h1>"
    "<p>Download WAV files directly to this device. No SD data is deleted."
    " When finished, reconnect to your normal network to import recordings into Synap.</p><ul>");
  DIR* directory=opendir(ODYSSEY_SD_RECORDING_DIR);
  size_t shown=0;
  if(directory) {
    struct dirent* entry=nullptr;
    while((entry=readdir(directory))!=nullptr && shown<250 &&
          !stopRequested.load() && uint32_t(millis()-startedAt)<MAX_MS) {
      const char* name=entry->d_name;
      if (!name || strlen(name)>55u) continue;
      char logical[64]{},full[96]{};
      snprintf(logical,sizeof(logical),"/synap/%s",name);
      if (!safeWav(logical) || !odysseySdPath(logical,full,sizeof(full)))continue;
      struct stat st{};
      if(stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<44)continue;
      char line[260];
      const int n=snprintf(line,sizeof(line),
        "<li><a href='/file?key=%s&amp;path=%s'>%s</a> (%lu bytes)</li>",
        current.key,logical,name,static_cast<unsigned long>(st.st_size));
      if(n>0 && size_t(n)<sizeof(line)){apServer->sendContent(String(line));++shown;}
      vTaskDelay(1);
    }
    closedir(directory);
  } else {
    apServer->sendContent("</ul><p>SD directory unavailable; no files changed.</p><ul>");
  }
  if(!shown)apServer->sendContent("<li>No compatible WAV files found.</li>");
  apServer->sendContent("</ul><p>Hotspot closes after 3 minutes idle, at most 15 minutes.</p>"
    "<form method=post action='/stop?key=");
  apServer->sendContent(String(current.key));
  apServer->sendContent("'><button>Finish downloads</button></form></html>");
  apServer->sendContent("");
  lastActivityAt=millis();
}
static void virtualHeader(uint8_t* h,uint32_t fileSize) {
  memset(h,0,44);
  const uint32_t pcm=fileSize-44u;
  memcpy(h,"RIFF",4);put32le(h+4,fileSize-8u);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,pcm);
}
static void file() {
  if(!authorized())return;
  const String path=apServer->arg("path");
  char full[96]{};
  if(!safeWav(path.c_str()) || !odysseySdPath(path.c_str(),full,sizeof(full))) {
    apServer->send(400,"text/plain","Invalid WAV path.");return;
  }
  struct stat st{};
  if(stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<44 ||
     uint64_t(st.st_size)>UINT32_MAX) {
    apServer->send(404,"text/plain","Recording is unavailable.");return;
  }
  FILE* input=fopen(full,"rb");
  if(!input){apServer->send(503,"text/plain","SD read unavailable.");return;}
  if(fseek(input,44,SEEK_SET)!=0) {
    fclose(input);apServer->send(503,"text/plain","SD seek failed.");return;
  }
  uint8_t* buffer=static_cast<uint8_t*>(malloc(2048));
  if(!buffer){fclose(input);apServer->send(503,"text/plain","Download buffer unavailable.");return;}
  const uint32_t fileSize=uint32_t(st.st_size);
  apServer->sendHeader("Content-Disposition",
    "attachment; filename=\""+path.substring(7)+"\"");
  apServer->setContentLength(fileSize);
  apServer->send(200,"audio/wav","");
  WiFiClient client=apServer->client();
  uint8_t header[44];virtualHeader(header,fileSize);
  size_t sent=client.write(header,sizeof(header));
  uint32_t remaining=fileSize-44u;
  while(sent==44 && remaining && client.connected() && !stopRequested.load() &&
        uint32_t(millis()-startedAt)<MAX_MS) {
    const size_t count=std::min(size_t(remaining),size_t(2048));
    const size_t got=fread(buffer,1,count,input);
    if(got!=count || client.write(buffer,got)!=got)break;
    remaining-=uint32_t(got);
    lastActivityAt=millis();
    vTaskDelay(1);
  }
  if(remaining || sent!=44)client.stop();
  free(buffer);fclose(input);lastActivityAt=millis();
}
static void shutDown() {
  if(apServer){apServer->stop();delete apServer;apServer=nullptr;}
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  store(Info{});
  apActive=false;
  stopRequested=false;
}
static void serveSession() {
  // One SD mutex lease across a session: no concurrent FAT operations,
  // offline recording, remounts, OTA, deletion or formatting.
  OdysseySdGuard sd(pdMS_TO_TICKS(1500));
  if(!sd || !odysseySdReady()){shutDown();return;}
  while(!stopRequested.load() &&
        uint32_t(millis()-startedAt)<MAX_MS &&
        uint32_t(millis()-lastActivityAt)<IDLE_MS &&
        !batteryCritical()) {
    apServer->handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  shutDown();
}
static void apTask(void*) {
  // Return from RAII scope before FreeRTOS self-deletion.
  serveSession();
  vTaskDelete(nullptr);
}
bool begin() {
  if(apActive.load() || OdysseyWifi::busy() || !odysseySdReady() ||
     odysseyRecording.load() || streamingEnabled.load() || otaBusy() ||
     sleepPending || batteryCritical())return false;
  Info next{};
  uint8_t randomBytes[32];esp_fill_random(randomBytes,sizeof(randomBytes));
  snprintf(next.ssid,sizeof(next.ssid),"Synap-C3-%02X%02X",randomBytes[0],randomBytes[1]);
  for(size_t i=0;i<16;++i) {
    snprintf(next.password+i*2,3,"%02x",randomBytes[i]);
    snprintf(next.key+i*2,3,"%02x",randomBytes[i+16]);
  }
  stopRequested=false;
  WiFi.persistent(false);
  if(!WiFi.mode(WIFI_AP) ||
     !WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),
                        IPAddress(255,255,255,0)) ||
     !WiFi.softAP(next.ssid,next.password,1,false,1)){
    WiFi.softAPdisconnect(true);WiFi.mode(WIFI_OFF);return false;
  }
  apServer=new(std::nothrow) WebServer(80);
  if(!apServer){WiFi.softAPdisconnect(true);WiFi.mode(WIFI_OFF);return false;}
  next.active=true;
  store(next);
  apServer->on("/",HTTP_GET,page);
  apServer->on("/file",HTTP_GET,file);
  apServer->on("/stop",HTTP_POST,[] {
    if(authorized()){
      apServer->send(200,"text/plain","Downloads finished. Return to Synap.");
      stopRequested=true;
    }
  });
  apServer->onNotFound([]{apServer->send(404,"text/plain","Open your private Synap link.");});
  apServer->begin();
  startedAt=lastActivityAt=millis();
  apActive=true;
  if(xTaskCreate(apTask,"c3-sd-ap",AP_STACK,nullptr,1,nullptr)!=pdPASS) {
    shutDown();return false;
  }
  return true;
}
bool stop() {
  if(!apActive.load()) return false;
  stopRequested=true;return true;
}
size_t encode(char* output,size_t capacity) {
  if(!output || capacity<32)return 0;
  const Info current=snapshot();
  const int n=current.active?
    snprintf(output,capacity,
      "{\"active\":true,\"ssid\":\"%s\",\"password\":\"%s\","
      "\"url\":\"http://192.168.4.1/?key=%s\",\"idleSeconds\":180}",
      current.ssid,current.password,current.key):
    snprintf(output,capacity,"{\"active\":false}");
  return n>0 && size_t(n)<capacity?size_t(n):0;
}
#else
bool busy(){return false;}
bool begin(){return false;}
bool stop(){return false;}
size_t encode(char* output,size_t cap){
  if(!output || cap<17)return 0;
  const int n=snprintf(output,cap,"{\"active\":false}");
  return n>0 && size_t(n)<cap?size_t(n):0;
}
#endif
} // namespace OdysseyWifiAp
#endif
