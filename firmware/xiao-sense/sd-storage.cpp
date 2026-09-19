// Media jobs serialize filesystem ownership. Never format a mounted card.
#include <SD.h>
#include <SPI.h>
namespace ChakshuStorage {
constexpr uint32_t RESERVE_BYTES=4u*1024u*1024u;
std::atomic<bool> ready{false};
uint64_t capacity=0,freeBytes=0;
uint32_t sequence=0,bootId=0;
char protectedStem[64]{};

void refresh() {
  capacity=ready?SD.totalBytes():0;
  const uint64_t used=ready?SD.usedBytes():0;
  freeBytes=used<=capacity?capacity-used:0;
}

bool begin(bool remount) {
  if (remount) ready=false;
  if (!ready) {
    SD.end();
    SPI.begin(7,8,9,21);
    // Older cards and expansion-board contacts may need a slower SPI clock.
    // Mount only; format_if_empty is always false.
    for(const uint32_t hz:{10000000u,4000000u,1000000u}) {
      ready=SD.begin(21,SPI,hz,"/sd",5,false) && SD.cardType()!=CARD_NONE;
      if(ready)break;
      SD.end();
    }
    if (ready && !SD.exists("/synap")) ready=SD.mkdir("/synap");
    if (!bootId) bootId=esp_random();
  }
  refresh();
  Serial.printf("[CHAKSHU] sd ready=%u total=%llu free=%llu\n",
    unsigned(ready),(unsigned long long)capacity,(unsigned long long)freeBytes);
  return ready;
}

bool recoverIO() {
  ready=false;
  SD.end();
  SPI.begin(7,8,9,21);
  // A card that mounts at 10 MHz can still fail sustained reads/writes because
  // of contact quality or long expansion-board traces. After a real I/O error,
  // recover at conservative clocks rather than immediately returning to 10 MHz.
  for(const uint32_t hz:{4000000u,1000000u}) {
    ready=SD.begin(21,SPI,hz,"/sd",5,false) && SD.cardType()!=CARD_NONE;
    if(ready)break;
    SD.end();
  }
  if(ready && !SD.exists("/synap")) ready=SD.mkdir("/synap");
  refresh();
  Serial.printf("[CHAKSHU] sd io-recovery ready=%u total=%llu free=%llu\n",
    unsigned(ready),(unsigned long long)capacity,(unsigned long long)freeBytes);
  return ready;
}

bool capturePath(const char* path) {
  if(!path || strncmp(path,"/synap/",7)!=0)return false;
  const char* name=path+7;
  if(strlen(name)<21)return false;
  // Synap captures are intentionally narrow: XXXXXXXX-XXXXXXXX.ext. Models,
  // temporary files and anything copied to the card by the user never match.
  for(int i=0;i<17;++i) {
    if(i==8){if(name[i]!='-')return false;continue;}
    const char c=name[i];
    if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
  }
  const char* ext=name+17;
  return !strcmp(ext,".jpg")||!strcmp(ext,".mjpeg")||!strcmp(ext,".wav")||!strcmp(ext,".json");
}

String stemFor(const char* path) {
  if(!capturePath(path))return String();
  String stem(path);
  const int dot=stem.lastIndexOf('.');
  if(dot<8)return String();
  stem.remove(dot);
  return stem;
}

void protect(const char* path) {
  const String stem=stemFor(path);
  if(!stem.length()){protectedStem[0]=0;return;}
  snprintf(protectedStem,sizeof(protectedStem),"%s",stem.c_str());
}
void clearProtection(){protectedStem[0]=0;}
bool isProtected(const String& stem){return stem.length() && protectedStem[0] && stem==protectedStem;}

bool removeIfPresent(const String& path) {
  if(!SD.exists(path.c_str()))return true;
  return SD.remove(path.c_str());
}

bool removeCapture(const char* path) {
  if(!ready || !capturePath(path))return false;
  const String stem=stemFor(path);
  if(!stem.length() || isProtected(stem))return false;
  const String mjpeg=stem+".mjpeg",wav=stem+".wav",json=stem+".json",jpg=stem+".jpg";
  bool ok=true;
  if(SD.exists(mjpeg.c_str()) || strstr(path,".mjpeg") || strstr(path,".json")) {
    ok=removeIfPresent(mjpeg)&&ok;
    ok=removeIfPresent(wav)&&ok;
    ok=removeIfPresent(json)&&ok;
  } else if(SD.exists(jpg.c_str()) || strstr(path,".jpg")) ok=removeIfPresent(jpg)&&ok;
  else ok=removeIfPresent(wav)&&ok; // standalone offline audio
  refresh();
  return ok;
}

bool oldestCapture(char* out,size_t length,bool includeEmpty=false) {
  if(!ready || !out || !length)return false;
  out[0]=0;
  File root=SD.open("/synap");
  if(!root || !root.isDirectory()){if(root)root.close();return false;}
  time_t oldestTime=0;
  String oldest;
  for(File file=root.openNextFile();file;file=root.openNextFile()) {
    if(file.isDirectory()){file.close();continue;}
    String path=file.path();
    const time_t written=file.getLastWrite();
    const size_t bytes=file.size();
    file.close();
    if(!capturePath(path.c_str()))continue;
    const String stem=stemFor(path.c_str());
    if(isProtected(stem))continue;
    // A zero-byte primary can be the file just opened by an in-progress capture.
    // FIFO never removes it. Manual Clear SD may remove stale empty captures.
    if(!includeEmpty && bytes==0)continue;
    // Treat a video bundle as one FIFO entry; its WAV/JSON companions never
    // compete independently with the primary MJPEG.
    if(path.endsWith(".json"))continue;
    if(path.endsWith(".wav") && SD.exists((stem+".mjpeg").c_str()))continue;
    if(oldest.length()==0 || (written && (!oldestTime || written<oldestTime)) ||
       (written==oldestTime && path<oldest)) {
      oldest=path;oldestTime=written;
    }
  }
  root.close();
  if(!oldest.length())return false;
  snprintf(out,length,"%s",oldest.c_str());
  return true;
}

bool ensureSpace(uint64_t expectedBytes=0) {
  if(!ready)return false;
  refresh();
  const uint64_t required=uint64_t(RESERVE_BYTES)+expectedBytes;
  if(required>capacity)return false;
  uint16_t removed=0;
  while(freeBytes<required) {
    char oldest[96];
    if(!oldestCapture(oldest,sizeof(oldest),false) || !removeCapture(oldest))return false;
    if(++removed>1000)return false;
  }
  if(removed)Serial.printf("[CHAKSHU] sd fifo removed=%u free=%llu required=%llu\n",
    unsigned(removed),(unsigned long long)freeBytes,(unsigned long long)required);
  return true;
}

uint16_t clearCaptures() {
  if(!ready)return 0;
  uint16_t removed=0;
  char oldest[96];
  while(oldestCapture(oldest,sizeof(oldest),true)) {
    if(!removeCapture(oldest))break;
    if(++removed==0xFFFF)break;
  }
  refresh();
  return removed;
}

File create(char* path,size_t length,const char* extension) {
  if(!ensureSpace())return File();
  for (uint8_t attempt=0;attempt<8;++attempt) {
    snprintf(path,length,"/synap/%08lx-%08lx.%s",
      (unsigned long)bootId,(unsigned long)++sequence,extension);
    if (!SD.exists(path)) return SD.open(path,FILE_WRITE);
  }
  return File();
}

void wavHeader(uint8_t* p,uint32_t bytes) {
  memset(p,0,44);
  memcpy(p,"RIFF",4);put32le(p+4,bytes+36);memcpy(p+8,"WAVEfmt ",8);
  put32le(p+16,16);p[20]=1;p[22]=1;put32le(p+24,16000);put32le(p+28,32000);
  p[32]=2;p[34]=16;memcpy(p+36,"data",4);put32le(p+40,bytes);
}
}
