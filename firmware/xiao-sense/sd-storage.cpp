// Media jobs serialize filesystem ownership. Never format a mounted card.
#include <SD.h>
#include <SPI.h>
namespace ChakshuStorage {
constexpr uint32_t RESERVE_BYTES=4u*1024u*1024u;
std::atomic<bool> ready{false};
uint64_t capacity=0,freeBytes=0;
uint32_t sequence=0,bootId=0;
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
File create(char* path,size_t length,const char* extension) {
  refresh();
  if (!ready || freeBytes<RESERVE_BYTES) return File();
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
