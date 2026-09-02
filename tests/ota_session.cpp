// g++ -std=c++17 -Wall -Wextra -Werror tests/ota_session.cpp -o /tmp/synap-ota-test
// Reproduce ESP32 core symbols before parsing the OTA header.
#define DISABLED 0x00
enum STATUS { OK = 0, FAIL, PENDING, BUSY };
#include "../firmware/dk_pendant_esp32s3/OtaSession.h"
#include <assert.h>
#include <vector>
#include <iostream>
using namespace Synap;
static_assert(DISABLED == 0x00, "Keep the ESP32 GPIO macro intact");
static_assert(Synap::OTA_DISABLED == 0 && Synap::LOCKED == 1 && Synap::FAILED == 6,
              "OTA state bytes must remain protocol-compatible");
struct Flash : OtaBackend {
  int begins=0,writes=0,commits=0,aborts=0;
  bool beginOK=true,writeOK=true,commitOK=true;
  OtaError finishResult=Synap::OK;
  OtaError authResult=Synap::OK;
  int authorizations=0;
  OtaError authorize(const uint8_t*,const uint8_t*,uint32_t) override {++authorizations;return authResult;}
  bool begin(uint32_t,const uint8_t*) override {++begins;return beginOK;}
  bool write(const uint8_t*,size_t) override {++writes;return writeOK;}
  OtaError finish() override {return finishResult;}
  bool commit() override {++commits;return commitOK;}
  void abort() override {++aborts;}
};
std::vector<uint8_t> command(uint8_t c,uint32_t session=7,size_t n=5) {
  std::vector<uint8_t> p(n);p[0]=c;OtaSession::put32(p.data()+1,session);return p;
}
void send(OtaSession& s,std::vector<uint8_t> p,uint32_t now=10,uint32_t connection=1,bool recording=false) {
  s.packet(p.data(),p.size(),now,connection,recording);
}
std::vector<uint8_t> begin(uint32_t size=80) {
  auto p=command(1,7,73);OtaSession::put32(p.data()+5,size);return p;
}
std::vector<uint8_t> data(uint32_t offset=0,size_t length=40) {
  auto p=command(2,7,9+length);OtaSession::put32(p.data()+5,offset);
  if(offset==0 && length>=36) {p[9]=0xE9;p[21]=9;OtaSession::put32(p.data()+41,0xABCD5432);}
  return p;
}
void ready(OtaSession& s) {
  s.configure(1024,173);send(s,begin());send(s,data());send(s,data(40));send(s,command(3));assert(s.state==READY);
}
int main() {
  {Flash f;OtaSession s(f);s.configure(1024,173);f.authResult=AUTH_FAILED;
   send(s,begin());assert(s.error==AUTH_FAILED && f.begins==0);
   f.authResult=AUTH_THROTTLED;send(s,begin());assert(s.error==AUTH_THROTTLED && f.begins==0);
   f.authResult=Synap::OK;send(s,begin(),10,1,true);assert(s.error==Synap::BUSY && f.begins==0);
   auto legacy=begin();legacy.resize(41);send(s,legacy);assert(s.error==BAD_PACKET && f.begins==0);
   send(s,begin());assert(s.state==RECEIVING);
   send(s,data(),10,2);assert(f.writes==0); // Authenticated transfer belongs to this connection.
   send(s,data());send(s,data());assert(s.offset==40 && f.writes==1);send(s,data(40));send(s,command(3));
   assert(s.state==READY && f.commits==0);send(s,command(4));assert(s.state==COMMITTED && f.commits==1);
   send(s,command(4));send(s,command(5));s.tick(90000,2,false);assert(s.state==COMMITTED && f.commits==1);}
  {Flash f;OtaSession s(f);ready(s);send(s,command(5));assert(s.state==FAILED && s.error==CANCELLED && f.commits==0);}
  for(OtaError failure:{HASH_MISMATCH,INVALID_IMAGE}) {Flash f;OtaSession s(f);f.finishResult=failure;
   s.configure(1024,173);send(s,begin());send(s,data());send(s,data(40));send(s,command(3));
   assert(s.error==failure && f.commits==0 && f.aborts>0);}
  {Flash f;OtaSession s(f);s.configure(64,173);send(s,begin());assert(s.error==BAD_SIZE && f.begins==0 && f.authorizations==0);}
  {Flash f;OtaSession s(f);s.configure(1024,63);assert(s.state==OTA_DISABLED);send(s,begin());assert(f.begins==0);}
  for(int scenario=0;scenario<9;++scenario) {Flash f;OtaSession s(f);s.configure(1024,173);send(s,begin());
    if(scenario==0) {send(s,data(1));assert(s.error==BAD_OFFSET);}
    if(scenario==1) {auto p=data();p[21]=0;send(s,p);assert(s.error==INVALID_IMAGE);}
    if(scenario==2) {send(s,data());auto p=data();p[10]=1;send(s,p);assert(s.error==BAD_OFFSET);}
    if(scenario==3) {send(s,command(3));assert(s.error==BAD_PACKET);}
    if(scenario==4) {s.tick(100,2,true);assert(s.error==LINK_LOST);}
    if(scenario==5) {s.tick(45011,1,true);assert(s.error==TIMED_OUT);}
    if(scenario==6) {f.writeOK=false;send(s,data());assert(s.error==FLASH_ERROR);}
    if(scenario==7) {send(s,data(0,81));assert(s.error==BAD_OFFSET);}
    if(scenario==8) {send(s,command(4));assert(s.error==BAD_PACKET);}
    assert(s.state==FAILED && f.commits==0);
  }
  {Flash f;OtaSession s(f);ready(s);send(s,command(4,9));assert(f.commits==0);s.tick(100,1,false);assert(s.error==LINK_LOST);}
  {Flash f;OtaSession s(f);s.configure(1024,173);send(s,begin(),UINT32_MAX-100);s.tick(100,1,true);
   assert(s.state==RECEIVING);s.tick(45000,1,true);assert(s.error==TIMED_OUT);}
  {Flash f;OtaSession s(f);ready(s);uint8_t status[20];s.status(status,502);
    assert(status[0]==0xD7 && status[1]==2 && status[2]==READY && OtaSession::u32(status+8)==80 && status[18]==246 && status[19]==1);}
  std::cout<<"PASS: OTA authentication gate, connection ownership, recording lock, ordering, duplicates, size/chip checks, hash/image failures, abort, timeout/wrap, disconnect and commit isolation\n";
}
