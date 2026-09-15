// Wi-Fi is powered only for an explicit, idle SD download session. The transfer
// worker retains the media lease, excluding recording, card remounts and OTA.
#include <WiFi.h>
#include <WebServer.h>
#include <new>
namespace ChakshuWifi {
constexpr uint32_t IDLE_MS=180000,MAX_MS=900000;
struct Info { bool active=false;char ssid[32]{},password[33]{},token[33]{}; };
Info info;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> stopRequested{false};
WebServer* server=nullptr;
uint32_t started=0,lastActivity=0;
Info copy() {Info out;portENTER_CRITICAL(&mux);out=info;portEXIT_CRITICAL(&mux);return out;}
void save(const Info& next) {portENTER_CRITICAL(&mux);info=next;portEXIT_CRITICAL(&mux);}
bool validPath(const String& path) {
  const size_t n=path.length();
  if(n<28 || n>31 || !path.startsWith("/synap/") || path[15]!='-' || path[24]!='.')return false;
  for(size_t i=7;i<24;++i)if(i!=15 && !((path[i]>='0'&&path[i]<='9')||(path[i]>='a'&&path[i]<='f')))return false;
  const String extension=path.substring(25);
  return extension=="jpg" || extension=="mjpeg" || extension=="wav" || extension=="json";
}
bool authorize() {
  server->sendHeader("Cache-Control","no-store");
  server->sendHeader("Referrer-Policy","no-referrer");
  server->sendHeader("X-Content-Type-Options","nosniff");
  server->sendHeader("Content-Security-Policy","default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'");
  const Info current=copy();
  if(!current.active || server->arg("key")!=current.token) {
    server->send(403,"text/plain","Open the private download link from Synap.");return false;
  }
  lastActivity=millis();return true;
}
void page() {
  if(!authorize())return;
  const Info current=copy();
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200,"text/html","");
  server->sendContent("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Chakshu downloads</title><style>body{font:17px system-ui;background:#f4f5f0;color:#182c24;max-width:42rem;margin:auto;padding:24px}h1{font-size:28px}li{padding:16px 0;border-bottom:1px solid #ccd5cb;overflow-wrap:anywhere}a{color:#245c44}button{font:inherit;padding:12px;border-radius:12px}small{display:block;margin-top:6px}p{line-height:1.5}</style><h1>Chakshu downloads</h1><p>Save photos, or the matching MJPEG, WAV and JSON files for each video. Return to Synap and choose Import SD files to play a video with its audio.</p><ul>");
  File directory=SD.open("/synap");unsigned count=0;
  for(File file=directory.openNextFile();file;file=directory.openNextFile()) {
    const String path=file.path();
    if(!file.isDirectory() && validPath(path)) {
      const String name=path.substring(7);
      server->sendContent("<li><a download href='/file?key="+String(current.token)+"&amp;path="+path+"'>"+name+"</a><small>"+String(file.size())+" bytes</small></li>");
      ++count;
    }
    file.close();
    if(count>=300 || stopRequested.load() || millis()-started>=MAX_MS)break;
    vTaskDelay(1);
  }
  directory.close();
  server->sendContent(count?"</ul>":"</ul><p>No saved files yet. Stop downloads and capture a photo or record to SD in Synap.</p>");
  server->sendContent("<p>This private network turns off after three minutes without a request, or after fifteen minutes.</p><form method=post action='/stop?key="+String(current.token)+"'><button>Finish downloads</button></form></html>");
  server->sendContent("");lastActivity=millis();
}
void file() {
  if(!authorize())return;
  const String path=server->arg("path");
  if(!validPath(path)){server->send(400,"text/plain","Invalid recording path.");return;}
  File input=SD.open(path,FILE_READ);
  if(!input || input.isDirectory()){input.close();server->send(404,"text/plain","File unavailable. Check the SD card.");return;}
  uint8_t* bytes=static_cast<uint8_t*>(malloc(4096));
  if(!bytes){input.close();server->send(503,"text/plain","Download memory unavailable. Try again.");return;}
  server->sendHeader("Content-Disposition","attachment; filename=\""+path.substring(7)+"\"");
  const size_t size=input.size();size_t sent=0;
  server->setContentLength(size);server->send(200,"application/octet-stream","");
  auto& client=server->client();
  // Check Stop/expiry between bounded writes, including a stalled downloader.
  while(sent<size && client.connected() && !stopRequested.load() && millis()-started<MAX_MS) {
    const size_t n=input.read(bytes,std::min(size_t(4096),size-sent));
    if(!n || client.write(bytes,n)!=n)break;
    sent+=n;lastActivity=millis();vTaskDelay(1);
  }
  if(sent!=size)client.stop();
  free(bytes);input.close();lastActivity=millis();
}
void end() {
  if(server){server->stop();delete server;server=nullptr;}
  WiFi.softAPdisconnect(true);WiFi.mode(WIFI_OFF);
  save(Info{});stopRequested=false;
}
bool begin() {
  if(!ChakshuStorage::ready || streamingEnabled.load())return false;
  Info next;uint8_t random[32];esp_fill_random(random,sizeof(random));
  snprintf(next.ssid,sizeof(next.ssid),"Chakshu-%02X%02X",random[0],random[1]);
  for(size_t i=0;i<16;++i) {
    snprintf(next.password+i*2,3,"%02x",random[i]);
    snprintf(next.token+i*2,3,"%02x",random[i+16]);
  }
  stopRequested=false;WiFi.persistent(false);
  if(!WiFi.mode(WIFI_AP) || !WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0)) ||
     !WiFi.softAP(next.ssid,next.password,1,false,1)) {end();return false;}
  server=new(std::nothrow) WebServer(80);
  if(!server){end();return false;}
  next.active=true;save(next);started=lastActivity=millis();
  server->on("/",HTTP_GET,page);
  server->on("/file",HTTP_GET,file);
  server->on("/stop",HTTP_POST,[]{if(authorize()){server->send(200,"text/plain","Downloads finished. Return to Synap.");stopRequested=true;}});
  server->onNotFound([]{server->send(404,"text/plain","Open the download link from Synap.");});
  server->begin();return true;
}
void serve() {
  while(!stopRequested.load() && millis()-lastActivity<IDLE_MS && millis()-started<MAX_MS) {
    server->handleClient();vTaskDelay(pdMS_TO_TICKS(5));
  }
  end();
}
size_t encode(char* output,size_t capacity) {
  const Info current=copy();
  const int n=current.active
    ?snprintf(output,capacity,"{\"active\":true,\"ssid\":\"%s\",\"password\":\"%s\",\"url\":\"http://192.168.4.1/?key=%s\",\"idleSeconds\":180}",current.ssid,current.password,current.token)
    :snprintf(output,capacity,"{\"active\":false}");
  return n>0 && size_t(n)<capacity?size_t(n):0;
}
}
