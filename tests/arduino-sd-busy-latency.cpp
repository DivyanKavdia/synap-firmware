#include <cassert>
#include <cstdint>
#include <deque>
#include <algorithm>

static uint32_t ticks=0;
static uint32_t millis() { return ticks++; }
static bool selected=false, delayed=true, busyForever=false, stopForever=false;
static bool timeoutObserved=false, reject=false;
static unsigned blocks=0, stops=0, statusChecks=0;
struct SPIClass {
  std::deque<uint8_t> input;
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
    if (delayed) input.push_back(0xff);
    input.push_back(0);input.push_back(0);input.push_back(0);
    if (forever) stuck=true;
    else input.push_back(0xff);
  }
  void write(uint8_t token) {
    assert(selected && !pendingBusy());
    if (token==0xfd) { ++stops;programming(stopForever); }
    else { assert(token==0xfe || token==0xfc);++blocks; }
  }
  void writeBytes(uint8_t*, int size) { assert(size==512); }
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
  assert(timeoutObserved || !spi.pendingBusy());
  selected=false;
}
static char sdCommand(uint8_t,int,unsigned long long,void*) {
  assert(selected && !spi.pendingBusy());return 0;
}
static char sdTransaction(uint8_t,int cmd,int,unsigned int* response) {
  assert(!selected && !spi.pendingBusy());
  if (cmd==SEND_STATUS) { ++statusChecks;*response=0; }
  return 0;
}

// INSERT DRIVER

static void reset(bool latency=true) {
  spi.input.clear();spi.stuck=false;ticks=0;selected=false;delayed=latency;
  busyForever=false;stopForever=false;timeoutObserved=false;reject=false;
  blocks=stops=statusChecks=0;synapSdClearWriteFaultCode();
}
int main() {
  char data[4096]{};
  for (bool latency : {false,true}) {
    reset(latency);
    assert(sdWriteSector(0,data,17));
    assert(blocks==1 && stops==0 && statusChecks==1 && !selected);
    assert(!synapSdWriteFaultCode());
    reset(latency);
    assert(sdWriteSectors(0,data,17,8));
    assert(blocks==8 && stops==1 && statusChecks==1 && !selected);
    assert(!synapSdWriteFaultCode());
  }
  reset();busyForever=true;
  assert(!sdWriteSector(0,data,17));
  assert(synapSdWriteFaultCode()==0x18040000u);
  assert(statusChecks==0 && timeoutObserved && ticks<5010);
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
  assert(synapSdWriteFaultCode()==0x1803000du);
  assert(blocks==1 && statusChecks==0);
  reset();reject=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x1905000du);
  assert(blocks==1 && stops==1 && statusChecks==0);
}
