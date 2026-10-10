#include <cassert>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <vector>
#include <cstring>

static uint32_t ticks=0;
static uint32_t millis() { return ticks++; }
static bool selected=false, busyForever=false, stopForever=false;
static bool timeoutObserved=false, reject=false;
static unsigned idleBeforeBusy=1;
static unsigned blocks=0, stops=0, statusChecks=0;
static int writeCommand=0;
static unsigned commandCount=0;
static unsigned long long writeAddress=0;
static bool failCommand=false,failStatus=false;
struct SPIClass {
  std::deque<uint8_t> input;
  std::vector<uint8_t> payload;
  bool stuck=false;
  uint8_t transfer(uint8_t) {
    assert(selected);
    if (!input.empty()) {
      const uint8_t value=input.front();input.pop_front();return value;
    }
    return stuck?0:0xff;
  }
  bool pendingBusy() const {
    return stuck || std::find(input.begin(),input.end(),0)!=input.end();
  }
  void programming(bool forever) {
    for (unsigned i=0;i<idleBeforeBusy;++i) input.push_back(0xff);
    input.push_back(0);input.push_back(0);input.push_back(0);
    if (forever) stuck=true;
    else input.push_back(0xff);
  }
  void write(uint8_t token) {
    assert(selected && !pendingBusy());
    if (token==0xfd) { ++stops;programming(stopForever); }
    else { assert(token==0xfe || token==0xfc);++blocks; }
  }
  void writeBytes(uint8_t* data, int size) {
    assert(size==512);payload.insert(payload.end(),data,data+size);
  }
  void write16(unsigned short) {
    input.push_back(reject?0x0d:0x05);
    if (!reject) programming(busyForever);
  }
};
static SPIClass spi;
enum { CARD_MMC=1, CARD_SDHC=3, WRITE_BLOCK_SINGLE=24,
  WRITE_BLOCK_MULTIPLE=25, SEND_STATUS=13, SET_WR_BLK_ERASE_COUNT=23 };
struct ardu_sdcard_t { SPIClass* spi; int type; bool supports_crc; };
static ardu_sdcard_t card{&spi,CARD_SDHC,false};
static ardu_sdcard_t* s_cards[]={&card};
static unsigned short CRC16(const char*,int) { return 0; }
template<typename... Args> static void log_e(const char*,Args...) {}
// The pinned Arduino wait stops at the FIRST nonzero byte. This is the
// behavior that makes an unconsumed response/STOP latency byte dangerous.
static bool sdWait(uint8_t pdrv,int timeout) {
  uint8_t resp;
  const uint32_t start=millis();
  do { resp=s_cards[pdrv]->spi->transfer(0xff); }
  while (resp==0 && (millis()-start)<static_cast<unsigned>(timeout));
  if (!resp) timeoutObserved=true;
  return resp>0;
}
static bool sdSelectCard(uint8_t) { selected=true;return true; }
static void sdDeselectCard(uint8_t) {
  assert(timeoutObserved || busyForever || stopForever || !spi.pendingBusy());
  selected=false;
}
static char sdCommand(uint8_t,int cmd,unsigned long long address,void*) {
  assert(selected && !spi.pendingBusy());
  writeCommand=cmd;writeAddress=address;++commandCount;
  return failCommand?4:0;
}
static char sdTransaction(uint8_t,int cmd,int,unsigned int* response) {
  assert(!selected && !spi.pendingBusy());
  if (cmd==SEND_STATUS) { ++statusChecks;*response=failStatus?1:0; }
  return 0;
}

// INSERT DRIVER

static void reset(unsigned idleBytes=1) {
  spi.input.clear();spi.stuck=false;ticks=0;selected=false;idleBeforeBusy=idleBytes;
  busyForever=false;stopForever=false;timeoutObserved=false;reject=false;
  blocks=stops=statusChecks=0;synapSdClearWriteFaultCode();
  writeCommand=0;writeAddress=0;commandCount=0;
  failCommand=failStatus=false;spi.payload.clear();card.type=CARD_SDHC;
}
int main() {
  char data[4096]{};
  // Include two transient nonzero bytes before busy; stock Arduino sdWait
  // would incorrectly treat the second as completed programming.
  for (unsigned idleBytes : {0u,1u,2u}) {
    reset(idleBytes);
    assert(sdWriteSector(0,data,17));
    assert(blocks==1 && stops==1 && statusChecks==1 && !selected);
    assert(writeCommand==25 && commandCount==1 && writeAddress==17);
    assert(!synapSdWriteFaultCode());
    reset(idleBytes);
    assert(sdWriteSectors(0,data,17,8));
    assert(blocks==8 && stops==1 && statusChecks==1 && !selected);
    assert(!synapSdWriteFaultCode());
  }
  reset();busyForever=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x19060100u);
  assert(statusChecks==0 && ticks<5010);
  reset();busyForever=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x19040100u);
  assert(blocks==1 && stops==0 && statusChecks==0 && ticks<5010);
  reset();stopForever=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x19070800u);
  assert(blocks==8 && stops==1 && statusChecks==0 && ticks<5100);
  reset();reject=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x1905000du);
  assert(blocks==1 && statusChecks==0);
  reset();reject=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x1905000du);
  assert(blocks==1 && stops==1 && statusChecks==0);

  // Metadata must transmit exactly the requested sector, not pad/overwrite
  // neighbouring FAT entries; SDSC retains byte addressing, SDHC block addressing.
  for (int type : {2,int(CARD_SDHC)}) {
    reset();card.type=type;
    for (unsigned i=0;i<sizeof(data);++i) data[i]=char(i*37u);
    assert(sdWriteSector(0,data+512,123));
    assert(spi.payload.size()==512 && !memcmp(spi.payload.data(),data+512,512));
    assert(writeAddress==(type==CARD_SDHC?123u:123u*512u));
    assert(writeCommand==25 && commandCount==1 && stops==1);
  }
  reset();stopForever=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x19070100u && commandCount==1);
  reset();failCommand=true;
  assert(!sdWriteSector(0,data,17));
  assert(commandCount==1 && blocks==0 && stops==0 && statusChecks==0);
  reset();failStatus=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x19080001u && commandCount==1);
  reset();
  assert(!sdWriteSectors(0,data,17,0));
  assert(commandCount==0 && blocks==0);
  reset();card.type=CARD_MMC;
  assert(sdWriteSector(0,data,17));
  assert(writeCommand==24 && blocks==1 && stops==0);
  reset();card.type=CARD_MMC;busyForever=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x18040000u);
}
