// Diagnostic-only C3 Wi-Fi size probe. This branch intentionally force-links
// Wi-Fi STA while omitting TLS/HTTP so CI reports the radio stack's flash cost.
// Never merge this module into production.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <WiFi.h>

namespace OdysseyWifi {
static std::atomic<bool> active{false};
static char staged[256]{};
static size_t stagedLength=0;

bool available(){return true;}
bool busy(){return active.load();}

uint32_t configChunk(uint32_t offset,const char* text) {
  const size_t n=text?strlen(text):0;
  if(offset==0){stagedLength=0;staged[0]=0;}
  if(offset!=stagedLength || !n || n>60 || stagedLength+n>=sizeof(staged))return UINT32_MAX;
  memcpy(staged+stagedLength,text,n);stagedLength+=n;staged[stagedLength]=0;
  return uint32_t(stagedLength);
}

bool applyConfig(uint32_t mode) {
  if(mode==1){
    Preferences prefs;
    if(!prefs.begin("c3-wifi",false))return false;
    const bool ok=prefs.putString("probe","1")>0;
    prefs.end();stagedLength=0;return ok;
  }
  if(mode!=2 || active.exchange(true))return false;
  WiFi.mode(WIFI_STA);
  // Force the full STA connection implementation into the link without using
  // credentials. This is a flash-size probe, not an executable transfer.
  WiFi.setSleep(false);
  WiFi.begin("SYNAP-SIZE-PROBE","size-probe-password");
  (void)WiFi.status();
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
  active=false;stagedLength=0;
  return false;
}

bool forget() {
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
  return true;
}

size_t encode(char* output,size_t capacity) {
  if(!output || capacity<32)return 0;
  const int n=snprintf(output,capacity,
    "{\"configured\":false,\"active\":%s,\"phase\":6,\"total\":0,\"uploaded\":0,\"http\":0,\"message\":\"size probe\"}",
    active.load()?"true":"false");
  return n>0&&size_t(n)<capacity?size_t(n):0;
}

void initialize(){
  // Calling mode() here ensures Wi-Fi generic initialization is retained by
  // linker GC even if the diagnostic BLE operation is never invoked.
  WiFi.mode(WIFI_OFF);
}
}
#endif
