#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Transport-independent protocol engine; only the control task calls these methods.
namespace Synap {
enum OtaState : uint8_t { DISABLED, LOCKED, ARMED, RECEIVING, READY, COMMITTED, FAILED };
enum OtaError : uint8_t { OK, NOT_ARMED, BAD_PACKET, BAD_SIZE, BAD_OFFSET, FLASH_ERROR,
  INVALID_IMAGE, HASH_MISMATCH, LINK_LOST, TIMED_OUT, CANCELLED, BUSY };
struct OtaBackend {
  virtual ~OtaBackend() = default;
  virtual bool begin(uint32_t size, const uint8_t* hash) = 0;
  virtual bool write(const uint8_t* data, size_t size) = 0;
  virtual OtaError finish() = 0;
  virtual bool commit() = 0;
  virtual void abort() = 0;
};
class OtaSession {
 public:
  static constexpr size_t PACKET_MAX = 182;
  OtaState state = DISABLED;
  OtaError error = OK;
  uint32_t session = 0, offset = 0, capacity = 0;
  uint16_t maxData = 0;
  explicit OtaSession(OtaBackend& backend) : backend(backend) {}
  static uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
  }
  static void put32(uint8_t* p, uint32_t n) { for (int i=0;i<4;++i) p[i]=uint8_t(n>>(8*i)); }
  bool busy() const { return state==RECEIVING || state==READY || state==COMMITTED; }
  void configure(uint32_t bytes, uint16_t data) {
    capacity=bytes;maxData=data;
    if (!busy()) state=capacity && maxData>=36 ? LOCKED : DISABLED;
  }
  void arm(uint32_t now, uint32_t connection) {
    if (!busy() && capacity && maxData>=36) {
      state=ARMED;error=OK;session=offset=0;owner=connection;last=now;
    }
  }
  void fail(OtaError reason) {
    backend.abort();state=FAILED;error=reason;lastLength=0;
  }
  void tick(uint32_t now, uint32_t connection, bool connected) {
    if (state==COMMITTED) return; // Boot selection is already committed; never claim cancellation.
    if (state==ARMED || busy()) {
      if (!connected || connection!=owner) { fail(LINK_LOST);return; }
      if (uint32_t(now-last) > (state==ARMED ? 90000u : 45000u)) {
        if (state==ARMED) { state=LOCKED;error=OK; }
        else fail(TIMED_OUT);
      }
    }
  }
  void packet(const uint8_t* p, size_t n, uint32_t now, uint32_t connection, bool recording) {
    if (state==COMMITTED) return;
    if (!p || n<5 || n>PACKET_MAX) { if (busy()) fail(BAD_PACKET);return; }
    const uint8_t command=p[0];const uint32_t id=u32(p+1);
    if (command==1) { // BEGIN: command, session, length, sha256.
      if (state!=ARMED || owner!=connection) { if (!busy()) error=NOT_ARMED;return; }
      if (recording) { error=BUSY;return; }
      if (n!=41 || !id) { error=BAD_PACKET;return; }
      const uint32_t bytes=u32(p+5);
      if (bytes<36 || bytes>capacity) { error=BAD_SIZE;return; }
      session=id;offset=0;size=bytes;error=OK;lastLength=0;
      if (!backend.begin(bytes,p+9)) { fail(FLASH_ERROR);return; }
      state=RECEIVING;last=now;return;
    }
    if (!busy() || connection!=owner || id!=session) return;
    if (command==5 && n==5) { fail(CANCELLED);return; }
    if (command==2 && state==RECEIVING && n>9 && n-9<=maxData) {
      const uint32_t position=u32(p+5);const size_t bytes=n-9;
      // Exactly one previous packet may be repeated after a lost application ACK.
      if (lastLength && position==lastOffset && bytes==lastLength &&
          memcmp(lastData,p+9,bytes)==0) { last=now;return; }
      if (position!=offset || bytes>size-offset) { fail(BAD_OFFSET);return; }
      if (offset==0 && (bytes<36 || p[9]!=0xE9 || p[21]!=9 || p[22]!=0 ||
          u32(p+9+32)!=0xABCD5432)) { fail(INVALID_IMAGE);return; }
      if (!backend.write(p+9,bytes)) { fail(FLASH_ERROR);return; }
      lastOffset=position;lastLength=bytes;memcpy(lastData,p+9,bytes);
      offset+=bytes;last=now;return;
    }
    if (command==3 && n==5 && state==RECEIVING && offset==size) {
      const OtaError result=backend.finish();
      if (result!=OK) { fail(result);return; }
      state=READY;last=now;return;
    }
    if (command==4 && n==5 && state==READY) {
      if (!backend.commit()) { fail(FLASH_ERROR);return; }
      state=COMMITTED;last=now;return;
    }
    fail(BAD_PACKET);
  }
  void status(uint8_t* p, uint16_t build) const {
    memset(p,0,20);p[0]=0xD7;p[1]=1;p[2]=state;p[3]=error;
    put32(p+4,session);put32(p+8,offset);put32(p+12,capacity);
    p[16]=maxData&255;p[17]=maxData>>8;p[18]=build&255;p[19]=build>>8;
  }
 private:
  OtaBackend& backend;
  uint32_t owner=0,last=0,size=0,lastOffset=0;
  size_t lastLength=0;
  uint8_t lastData[PACKET_MAX-9]{};
};
}
