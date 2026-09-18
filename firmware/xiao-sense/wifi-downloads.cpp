// Wi-Fi is powered only for an explicit, idle SD download session. The transfer
// worker retains the media lease, excluding recording, card remounts and OTA.
// A tiny HTTP/1.1 handler avoids pulling the full Arduino WebServer stack into
// the space-constrained Chakshu OTA image while keeping the same download URLs.
#include <WiFi.h>
#include <new>
namespace ChakshuWifi {
constexpr uint32_t IDLE_MS=180000,MAX_MS=900000;
struct Info { bool active=false;char ssid[32]{},password[33]{},token[33]{}; };
Info info;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> stopRequested{false};
WiFiServer* server=nullptr;
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

String queryValue(const String& target,const char* name) {
  const int query=target.indexOf('?');
  if(query<0)return String();
  const String key=String(name)+"=";
  int at=query+1;
  while(at<int(target.length())) {
    const int end=target.indexOf('&',at);
    const int stop=end<0?target.length():end;
    if(target.startsWith(key,at))return target.substring(at+key.length(),stop);
    if(end<0)break;
    at=end+1;
  }
  return String();
}

bool readLine(WiFiClient& client,String& out) {
  out="";
  const uint32_t began=millis();
  while(client.connected() && millis()-began<1500u) {
    while(client.available()) {
      const char c=char(client.read());
      if(c=='\n') {
        if(out.endsWith("\r"))out.remove(out.length()-1);
        return true;
      }
      if(out.length()>=511)return false;
      out+=c;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return false;
}

bool readRequest(WiFiClient& client,String& method,String& target) {
  String line;
  if(!readLine(client,line))return false;
  const int first=line.indexOf(' '),second=first<0?-1:line.indexOf(' ',first+1);
  if(first<=0||second<=first+1||!line.substring(second+1).startsWith("HTTP/1."))return false;
  method=line.substring(0,first);target=line.substring(first+1,second);
  for(unsigned n=0;n<32;++n) {
    if(!readLine(client,line))return false;
    if(!line.length())return true;
  }
  return false;
}

void commonHeaders(WiFiClient& client,const char* status,const char* type,int64_t length=-1,const char* disposition=nullptr) {
  client.printf("HTTP/1.1 %s\r\n",status);
  client.printf("Content-Type: %s\r\n",type);
  if(length>=0)client.printf("Content-Length: %lld\r\n",(long long)length);
  if(disposition)client.printf("Content-Disposition: %s\r\n",disposition);
  client.print("Connection: close\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n");
  client.print("X-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'\r\n\r\n");
}

void textReply(WiFiClient& client,const char* status,const char* body) {
  commonHeaders(client,status,"text/plain; charset=utf-8",strlen(body));
  client.write(reinterpret_cast<const uint8_t*>(body),strlen(body));
}

bool authorize(WiFiClient& client,const String& target) {
  const Info current=copy();
  if(!current.active || queryValue(target,"key")!=current.token) {
    textReply(client,"403 Forbidden","Open the private download link from Synap.");
    return false;
  }
  lastActivity=millis();return true;
}

void page(WiFiClient& client,const String& target) {
  if(!authorize(client,target))return;
  const Info current=copy();
  commonHeaders(client,"200 OK","text/html; charset=utf-8");
  client.print("<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Chakshu downloads</title><style>body{font:17px system-ui;background:#f4f5f0;color:#182c24;max-width:42rem;margin:auto;padding:24px}h1{font-size:28px}li{padding:16px 0;border-bottom:1px solid #ccd5cb;overflow-wrap:anywhere}a{color:#245c44}button{font:inherit;padding:12px;border-radius:12px}small{display:block;margin-top:6px}p{line-height:1.5}</style><h1>Chakshu downloads</h1><p>Save photos, or the matching MJPEG, WAV and JSON files for each video. Return to Synap and choose Import SD files to play a video with its audio.</p><ul>");
  File directory=SD.open("/synap");unsigned count=0;
  for(File file=directory.openNextFile();file;file=directory.openNextFile()) {
    const String path=file.path();
    if(!file.isDirectory() && validPath(path)) {
      const String name=path.substring(7);
      client.print("<li><a download href='/file?key="+String(current.token)+"&amp;path="+path+"'>"+name+"</a><small>"+String(file.size())+" bytes</small></li>");
      ++count;
    }
    file.close();
    if(count>=300 || stopRequested.load() || millis()-started>=MAX_MS || !client.connected())break;
    vTaskDelay(1);
  }
  directory.close();
  if(!count)client.print("</ul><p>No saved files yet. Stop downloads and capture a photo or record to SD in Synap.</p>");
  else client.print("</ul>");
  client.print("<p>This private network turns off after three minutes without a request, or after fifteen minutes.</p><form method=post action='/stop?key="+String(current.token)+"'><button>Finish downloads</button></form></html>");
  lastActivity=millis();
}

void file(WiFiClient& client,const String& target) {
  if(!authorize(client,target))return;
  const String path=queryValue(target,"path");
  if(!validPath(path)){textReply(client,"400 Bad Request","Invalid recording path.");return;}
  File input=SD.open(path,FILE_READ);
  if(!input || input.isDirectory()){input.close();textReply(client,"404 Not Found","File unavailable. Check the SD card.");return;}
  uint8_t* bytes=static_cast<uint8_t*>(malloc(4096));
  if(!bytes){input.close();textReply(client,"503 Service Unavailable","Download memory unavailable. Try again.");return;}
  const String disposition="attachment; filename=\""+path.substring(7)+"\"";
  const size_t size=input.size();size_t sent=0;
  commonHeaders(client,"200 OK","application/octet-stream",size,disposition.c_str());
  while(sent<size && client.connected() && !stopRequested.load() && millis()-started<MAX_MS) {
    const size_t n=input.read(bytes,std::min(size_t(4096),size-sent));
    if(!n || client.write(bytes,n)!=n)break;
    sent+=n;lastActivity=millis();vTaskDelay(1);
  }
  free(bytes);input.close();lastActivity=millis();
}

void handle(WiFiClient& client) {
  client.setTimeout(1500);
  String method,target;
  if(!readRequest(client,method,target)){textReply(client,"400 Bad Request","Invalid request.");return;}
  if(method=="GET" && (target=="/"||target.startsWith("/?"))){page(client,target);return;}
  if(method=="GET" && target.startsWith("/file?")){file(client,target);return;}
  if(method=="POST" && target.startsWith("/stop?")) {
    if(authorize(client,target)){textReply(client,"200 OK","Downloads finished. Return to Synap.");stopRequested=true;}
    return;
  }
  textReply(client,"404 Not Found","Open the download link from Synap.");
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
  server=new(std::nothrow) WiFiServer(80);
  if(!server){end();return false;}
  server->begin();next.active=true;save(next);started=lastActivity=millis();return true;
}

void serve() {
  while(!stopRequested.load() && millis()-lastActivity<IDLE_MS && millis()-started<MAX_MS) {
    WiFiClient client=server->available();
    if(client){handle(client);client.stop();}
    else vTaskDelay(pdMS_TO_TICKS(5));
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
