#include <cassert>
#include <cstdio>
#include <vector>
using namespace Synap;
constexpr const char* ID="SYNAP-AABBCCDDEEFF";
struct Flash : OtaBackend {
  unsigned begins=0,writes=0,commits=0;OtaError result=OK;
  bool matchesDevice(const uint8_t* id) override {return memcmp(id,ID,18)==0;}
  bool begin(uint32_t,const uint8_t*) override {++begins;return true;}
  bool write(const uint8_t*,size_t) override {++writes;return true;}
  OtaError finish() override {return result;}
  bool commit() override {++commits;return true;}
  void abort() override {}
};
std::vector<uint8_t> packet(uint8_t command,size_t n=5){
  std::vector<uint8_t> p(n);p[0]=command;OtaSession::put32(p.data()+1,7);return p;
}
std::vector<uint8_t> begin(const char* id=ID){
  auto p=packet(1,59);OtaSession::put32(p.data()+5,64);memcpy(p.data()+41,id,18);return p;
}
std::vector<uint8_t> data(){
  auto p=packet(2,73);p[9]=0xe9;p[21]=9;OtaSession::put32(p.data()+9+32,0xabcd5432);return p;
}
void send(OtaSession& s,const std::vector<uint8_t>& p,uint32_t connection=1,bool recording=false){s.packet(p.data(),p.size(),100,connection,recording);}
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
  send(t,begin());t.tick(50000,1,true);assert(t.error==TIMED_OUT&&g.commits==0);
  send(t,begin());t.tick(200,2,true);assert(t.error==LINK_LOST&&g.commits==0);
  send(t,begin());send(t,packet(5));assert(t.error==CANCELLED&&g.commits==0);
  puts("PASS: device target, legacy rejection, idle guard, ordering, duplicate, commit, hash failure, timeout, disconnect and cancellation");
}
