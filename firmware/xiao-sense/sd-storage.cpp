// Media jobs serialize filesystem ownership. Never format a mounted card.
#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>
namespace ChakshuStorage {
constexpr uint32_t RESERVE_BYTES=4u*1024u*1024u;
constexpr uint8_t SD_SCK=7,SD_MISO=8,SD_MOSI=9,SD_CS=21;
std::atomic<bool> ready{false};
uint64_t capacity=0,freeBytes=0;
uint32_t sequence=0,bootId=0;
char protectedStem[64]{};
uint32_t clockHz=10000000u;
uint32_t mountAttempts=0;
const char* mountStage="not-started";
bool recoveryClockLocked=false;
uint8_t probeCardType=CARD_NONE;
bool probeSector0=false,probeBootSignature=false,probeFilesystem=false;

void clearProbe() {
  probeCardType=CARD_NONE;probeSector0=false;probeBootSignature=false;probeFilesystem=false;
}

bool prepareBus() {
  ready=false;mountStage="spi";
  SD.end();
  SPI.end();
  // The Sense slot shares GPIO21 with the board's user LED. For SD it must
  // remain an inactive-high chip select; no status path may pulse this pin.
  pinMode(SD_CS,OUTPUT);
  digitalWrite(SD_CS,HIGH);
  delay(2);
  if(!SPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS)){SPI.end();return false;}
  return true;
}

void refresh() {
  capacity=ready?SD.totalBytes():0;
  const uint64_t used=ready?SD.usedBytes():0;
  freeBytes=used<=capacity?capacity-used:0;
}

bool validateMount(uint32_t hz) {
  ++mountAttempts;
  // Record the clock being attempted, not only one that worked. Reporting the
  // last successful clock leaves a card that has never mounted reporting the
  // initial 10 MHz for ever, which reads in the field as "the ladder never
  // stepped down" when in fact it ran all three and every one failed. That
  // misreading cost a debugging session.
  clockHz=hz;
  mountStage="mount";
  bool usable=SD.begin(SD_CS,SPI,hz,"/sd",5,false);
  // Separate no handshake at all from a bus that answered with no card behind
  // it. Both used to report "mount", so neither could be told apart without
  // opening the device.
  if(!usable)mountStage="mount-failed";
  else if(SD.cardType()==CARD_NONE){usable=false;mountStage="no-card";}
  if(usable){mountStage="directory";if(!SD.exists("/synap"))usable=SD.mkdir("/synap");}
  if(usable) {
    File directory=SD.open("/synap");
    usable=directory && directory.isDirectory();
    directory.close();
  }
  // A successful mount alone does not prove the filesystem is readable.
  if(usable){mountStage="capacity";usable=SD.totalBytes()>0;}
  ready=usable;
  if(usable)mountStage="ready";
  else SD.end();
  Serial.printf("[CHAKSHU] sd mount hz=%lu ready=%u heap=%lu\n",
    (unsigned long)hz,unsigned(ready),(unsigned long)ESP.getFreeHeap());
  return ready;
}

void inspectFailedMount() {
  // SD.begin() collapses card-initialisation and FAT-mount failures into one
  // boolean. After all normal attempts fail, use Arduino-ESP32's pinned SD
  // driver directly to preserve the card state long enough to tell those
  // failures apart. This probe is read-only: format_if_empty=false and the only
  // sector operation is a read of sector zero.
  clearProbe();
  if(!prepareBus())return;
  mountStage="probe";
  const uint8_t drive=sdcard_init(SD_CS,&SPI,400000);
  if(drive==0xFF){mountStage="driver";SPI.end();return;}
  probeFilesystem=sdcard_mount(drive,"/sd-probe",1,false);
  probeCardType=uint8_t(sdcard_type(drive));
  uint8_t sector[512]{};
  if(probeCardType!=CARD_NONE && probeCardType!=CARD_UNKNOWN) {
    probeSector0=sd_read_raw(drive,sector,0);
    probeBootSignature=probeSector0 && sector[510]==0x55 && sector[511]==0xAA;
  }
  sdcard_unmount(drive);
  sdcard_uninit(drive);
  SPI.end();
  if(probeFilesystem)mountStage="probe-mounted";
  else if(probeSector0)mountStage="filesystem";
  else if(probeCardType!=CARD_NONE)mountStage="card-read";
  else mountStage="card";
}

bool resetMountAt(uint32_t hz) {
  // Every attempt starts from a deselected card and a fresh Sense-pin SPI bus.
  // This avoids carrying a wedged card/SPI transaction into the next clock.
  if(!prepareBus())return false;
  if(validateMount(hz))return true;
  digitalWrite(SD_CS,HIGH);
  SPI.end();
  delay(2);
  return false;
}

bool detectCard() {
  clearProbe();
  // 400 kHz is intentionally retained as the last-resort operating clock. The
  // SD driver also uses a slow clock during card initialization, but marginal
  // links can still fail once filesystem traffic begins.
  for(const uint32_t hz:{10000000u,4000000u,1000000u,400000u}) {
    if(resetMountAt(hz))return true;
  }
  inspectFailedMount();
  return false;
}

bool recoverMount() {
  // Once a mounted card has shown an I/O fault, never raise it above 1 MHz for
  // the rest of this boot. Some cards that are unstable at 1 MHz remain fully
  // usable at 400 kHz, so preserve that final recovery rung as well.
  for(const uint32_t hz:{1000000u,400000u}) {
    if(resetMountAt(hz))return true;
  }
  return false;
}

bool begin(bool remount) {
  if(remount || !ready) {
    if(recoveryClockLocked)recoverMount();
    else detectCard();
    if(!bootId)bootId=esp_random();
  }
  refresh();
  return ready;
}

bool recoverIO() {
  // A real mounted-card I/O failure locks recovery to conservative clocks for
  // this boot. It never returns to 10 or 4 MHz without a restart.
  recoveryClockLocked=true;
  recoverMount();
  refresh();
  return ready;
}

size_t diagnostics(uint8_t* bytes,size_t length) {
  const int size=snprintf(reinterpret_cast<char*>(bytes),length,
    "{\"sdReady\":%s,\"sdClockHz\":%lu,\"sdMountStage\":\"%s\",\"sdMountAttempts\":%lu,\"sdRecoveryLocked\":%s,\"sdCsPin\":%u,\"sdProbeCardType\":%u,\"sdProbeSector0\":%s,\"sdProbeBootSignature\":%s,\"sdProbeFilesystem\":%s,\"resetReason\":%u,\"uptimeMs\":%lu,\"freeHeap\":%lu}",
    ready?"true":"false",(unsigned long)clockHz,mountStage,
    (unsigned long)mountAttempts,recoveryClockLocked?"true":"false",unsigned(SD_CS),
    unsigned(probeCardType),probeSector0?"true":"false",probeBootSignature?"true":"false",
    probeFilesystem?"true":"false",unsigned(esp_reset_reason()),(unsigned long)millis(),
    (unsigned long)ESP.getFreeHeap());
  return size>0 && size_t(size)<length?size_t(size):0;
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
  } else if(SD.exists(jpg.c_str()) || strstr(path,".jpg")) {
    ok=removeIfPresent(json)&&ok; // optional offline describe marker
    ok=removeIfPresent(jpg)&&ok;
  } else ok=removeIfPresent(wav)&&ok; // standalone offline audio
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
  // Anything still on the card is unsynced. Never reclaim it automatically:
  // the PWA deletes a capture only after the imported copy has been verified.
  // If the user stays offline long enough to fill the card, fail the new
  // capture with NO_SPACE rather than silently destroy an older memory.
  if(freeBytes<required) {
    Serial.printf("[CHAKSHU] sd full free=%llu required=%llu; unsynced media preserved\n",
      (unsigned long long)freeBytes,(unsigned long long)required);
    return false;
  }
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
