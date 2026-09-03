#include <cassert>
#include <cstdio>
#include <vector>
using namespace Synap;
constexpr const char* ID="SYNAP-AABBCCDDEEFF";
struct Flash : OtaBackend {
  unsigned begins=0,writes=0,commits=0,aborts=0;OtaError result=OK;
  bool matchesDevice(const uint8_t* id) override {return memcmp(id,ID,18)==0;}
  bool begin(uint32_t,const uint8_t*) override {++begins;return true;}
  bool write(const uint8_t*,size_t) override {++writes;return true;}
  OtaError finish() override {return result;}
  bool commit() override {++commits;return true;}
  void abort() override {++aborts;}
};
std::vector<uint8_t> packet(uint8_t command,size_t n=5){
  std::vector<uint8_t> p(n);p[0]=command;OtaSession::put32(p.data()+1,7);return p;
}
std::vector<uint8_t> begin(const char* id=ID){
  auto p=packet(1,59);OtaSession::put32(p.data()+5,64);memcpy(p.data()+41,id,18);return p;
}
std::vector<uint8_t> resume(const char* id=ID){
  auto p=packet(6,59);OtaSession::put32(p.data()+5,64);memcpy(p.data()+41,id,18);return p;
}
std::vector<uint8_t> data(){
  auto p=packet(2,73);p[9]=0xe9;p[21]=9;OtaSession::put32(p.data()+9+32,0xabcd5432);return p;
}
void sendAt(OtaSession& s,const std::vector<uint8_t>& p,uint32_t now,uint32_t connection=1,bool recording=false){s.packet(p.data(),p.size(),now,connection,recording);}
void send(OtaSession& s,const std::vector<uint8_t>& p,uint32_t connection=1,bool recording=false){sendAt(s,p,100,connection,recording);}
int main(){
  Flash f;OtaSession s(f);s.configure(2048,173);
  uint8_t status[20];s.status(status,1004);assert(status[1]==3);
  send(s,begin("SYNAP-112233445566"));assert(s.error==DEVICE_MISMATCH&&f.begins==0);
  auto legacy=begin();legacy.resize(73);send(s,legacy);assert(s.error==BAD_PACKET&&f.begins==0);
  send(s,begin(),1,true);assert(s.error==BUSY&&f.begins==0);
  send(s,begin());assert(s.state==RECEIVING&&f.begins==1);
  send(s,data(),2);assert(s.offset==0&&f.writes==0); // Other connection cannot feed this session.
  send(s,data());assert(s.offset==64&&f.writes==1);
  send(s,data());assert(s.offset==64&&f.writes==1); // Exact duplicate acknowledged, never rewritten.
  send(s,packet(3));assert(s.state==READY);
  send(s,packet(4));assert(s.state==COMMITTED&&f.commits==1);
  s.tick(50000,2,false);send(s,packet(5));assert(s.state==COMMITTED); // Never cancel a committed image.

  Flash g;OtaSession t(g);t.configure(2048,173);send(t,begin());
  auto outOfOrder=data();OtaSession::put32(outOfOrder.data()+5,1);send(t,outOfOrder);assert(t.error==BAD_OFFSET&&g.commits==0);
  send(t,begin());send(t,data());g.result=HASH_MISMATCH;send(t,packet(3));assert(t.state==FAILED&&t.error==HASH_MISMATCH&&g.commits==0);

  // A phone can suspend the browser while BLE remains logically connected. The
  // prepared production engine must retain the flash handle and exact offset for
  // the full recovery window instead of reproducing the old 45-second abort.
  Flash h;OtaSession u(h);u.configure(2048,503);send(u,begin());send(u,data());
  const unsigned abortsBeforePause=h.aborts;
  u.tick(600000,1,true);assert(u.state==RECEIVING&&u.error==OK&&u.offset==64&&h.aborts==abortsBeforePause);
  u.tick(899999,1,true);assert(u.state==RECEIVING&&u.offset==64&&h.aborts==abortsBeforePause);
  u.tick(900101,1,true);assert(u.state==FAILED&&u.error==TIMED_OUT&&h.aborts==abortsBeforePause+1);

  // A real disconnect gets the same bounded retention window and can be rebound
  // by a matching session/hash/device RESUME without erasing already-written data.
  Flash j;OtaSession v(j);v.configure(2048,503);send(v,begin());send(v,data());
  v.tick(200,2,false);v.tick(899999,2,false);assert(v.state==RECEIVING&&v.offset==64);
  send(v,resume(),2);assert(v.state==RECEIVING&&v.offset==64&&v.error==OK);
  send(v,packet(3),2);send(v,packet(4),2);assert(v.state==COMMITTED&&j.commits==1);

  Flash k;OtaSession w(k);w.configure(2048,503);send(w,begin());
  w.tick(200,2,false);w.tick(900201,2,false);assert(w.state==FAILED&&w.error==LINK_LOST); // Bounded cleanup.

  Flash m;OtaSession x(m);x.configure(2048,503);send(x,begin());send(x,packet(5));
  assert(x.error==CANCELLED&&m.commits==0);
  puts("PASS: device target, ordering, duplicate, commit, hash failure, 15-minute phone-lock retention, reconnect resume and bounded cleanup");
}
