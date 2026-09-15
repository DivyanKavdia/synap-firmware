#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
namespace ChakshuMedia {
struct Request { uint32_t connection;uint8_t operation,id; };
struct Snapshot { uint8_t operation=0,id=0,state=0,error=0,ready=0,progress=0;uint32_t bytes=0,connection=0;char path[64]{}; };
Snapshot status;
std::atomic<bool> busy{false},deviceConnected{true},streamingEnabled{false};
std::atomic<uint32_t> connectionGeneration{7};
bool updating=false,remoteStandby=false,full=false;
bool otaBusy(){return updating;}
enum { BUSY=1,OK=0,pdTRUE=1 };
std::deque<Request> input,output;
auto* requests=&input;
auto* jobs=&output;
int xQueueReceive(std::deque<Request>* queue,Request* result,int) {
  if(queue->empty())return 0;
  *result=queue->front();queue->pop_front();return pdTRUE;
}
int xQueueSend(std::deque<Request>* queue,const Request* request,int) {
  if(full)return 0;
  queue->push_back(*request);
  return pdTRUE;
}
void copy(Snapshot& value){value=status;}
void save(const Snapshot& value){status=value;}
// INSERT MEDIA TICK
}
int main() {
  using namespace ChakshuMedia;
  input.push_back({6,3,1});tick();assert(status.id==0&&!busy&&output.empty());
  streamingEnabled=true;input.push_back({7,3,1});tick();
  assert(status.id==1&&status.state==3&&status.error==BUSY&&!busy&&output.empty());
  streamingEnabled=false;updating=true;input.push_back({7,2,2});tick();
  assert(status.id==2&&status.error==BUSY&&!busy&&output.empty());
  updating=false;input.push_back({7,3,3});tick();
  assert(status.id==3&&status.state==1&&busy&&output.size()==1);
  input.push_back({7,4,4});tick();
  assert(status.id==3&&busy&&output.size()==1);
  busy=false;status.state=2;status.bytes=320000;
  input.push_back({7,3,3});tick();
  assert(status.state==2&&status.bytes==320000&&!busy&&output.size()==1);
  // A new connection restarts its transaction IDs at one; it must still capture.
  connectionGeneration=8;input.push_back({8,3,3});tick();
  assert(status.state==1&&status.connection==8&&busy&&output.size()==2);
  busy=false;connectionGeneration=7;
  full=true;input.push_back({7,2,4});tick();
  assert(status.id==4&&status.state==3&&status.error==BUSY&&!busy);
  // BUSY was never admitted, so retrying the same ID can now proceed.
  full=false;input.push_back({7,2,4});tick();
  assert(status.state==1&&status.error==OK&&busy&&output.size()==3);busy=false;
  deviceConnected=false;input.push_back({7,3,5});tick();
  assert(status.id==4&&output.size()==3);
  std::puts("PASS media ownership, stale-link rejection, audio/OTA exclusion, idempotency and queue failure");
}
