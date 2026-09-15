'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');
test('local commands hand off to a leased page or save separate offline modes',()=>{
  const tick=source.slice(source.indexOf('void tick() {'),source.indexOf('class Callbacks'));
  const fixture=`#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
constexpr int pdTRUE=1;
uint32_t now=1000;uint32_t millis(){return now;}
bool updating=false;bool otaBusy(){return updating;}
bool listening=true;bool active(){return listening;}
std::atomic<int> persistEnabled{-1};
struct Preferences{bool begin(const char*,bool){return true;}void putBool(const char*,bool){}void end(){}};
struct PendingCommand { uint8_t action;uint32_t epoch,at; };
std::atomic<uint32_t> discontinuities{0};
std::vector<PendingCommand> commandStorage;auto* commandQueue=&commandStorage;
int xQueueReceive(std::vector<PendingCommand>* q,PendingCommand* action,int){if(q->empty())return 0;*action=q->front();q->erase(q->begin());return pdTRUE;}
std::atomic<bool> deviceConnected{false},streamingEnabled{false};
std::atomic<uint32_t> connectionGeneration{1},leaseConnection{1},leaseAt{0};
unsigned stops=0;void stopStreaming(){++stops;streamingEnabled=false;}
bool exitRemoteStandby(){return true;}
namespace ChakshuTransfer {
struct Request{bool local=false;uint32_t localEpoch=0;uint8_t operation=0;};
std::atomic<bool> offline{false},stopRequested{false},photoRequested{false};
std::atomic<uint8_t> offlineMode{0};std::atomic<uint32_t> localEpoch{0};
std::vector<Request> storage;auto* requests=&storage;
}
int xQueueSend(std::vector<ChakshuTransfer::Request>* q,const ChakshuTransfer::Request* r,int){if(q->size()>=2)return 0;q->push_back(*r);return pdTRUE;}
int stateMux=0;void portENTER_CRITICAL(int*){}void portEXIT_CRITICAL(int*){}
uint32_t serial=0,lastAt=0;uint8_t lastCommand=0,lastResult=0;
struct Event{int notifications=0;void setValue(uint8_t*,int){}void notify(){++notifications;}} event;Event* events=&event;
void encode(uint8_t*){}
${tick}
void command(uint8_t action){commandStorage.push_back({action,discontinuities.load(),now});tick();}
int main(){using namespace ChakshuTransfer;
  command(4);assert(storage.size()==1&&storage.back().operation==10&&storage.back().local);
  storage.clear();offline=true;offlineMode=10;command(4);assert(storage.empty());
  command(2);assert(stopRequested&&storage.size()==1&&storage.back().operation==5);
  const auto oldEpoch=storage.back().localEpoch;command(5);assert(localEpoch!=oldEpoch&&stopRequested);
  // A photo uses the owning SD worker during a take, never a second camera reader.
  command(1);assert(photoRequested);
  offline=false;storage.clear();deviceConnected=true;leaseAt=100;leaseConnection=1;
  command(2);assert(storage.empty()&&lastResult==2&&event.notifications==1);
  leaseConnection=0;command(1);assert(storage.size()==1&&storage.back().operation==11);
  storage.clear();leaseConnection=1;now=6100;streamingEnabled=true;
  command(3);assert(stops==1&&!streamingEnabled&&lastResult==0);
  // A full local queue does not report that a mode was started.
  storage.resize(2);command(2);assert(lastResult==1);
  storage.clear();updating=true;command(2);assert(storage.empty());updating=false;
  listening=false;command(2);assert(storage.empty());
  listening=true;commandStorage.push_back({2,discontinuities.load(),now});++discontinuities;tick();assert(storage.empty());
  commandStorage.push_back({2,discontinuities.load(),now-2001});tick();assert(storage.empty());
  puts("PASS local/online routing and distinct audio/video operations");
}`;
  assert.match(nativeTest(fixture),/PASS local\/online routing/);
});
test('idle command listener yields microphone ownership to both recording consumers',()=>{
  const idle=source.slice(source.indexOf('void idleTask(void*) {'),source.indexOf('void cleanup()'));
  const fixture=`#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
constexpr int pdTRUE=1,portMAX_DELAY=0;struct Done{};
void ulTaskNotifyTake(int,int){}int pdMS_TO_TICKS(int n){return n;}void vTaskDelay(int){throw Done{};}
std::atomic<bool> streamingEnabled{false},otaBusySnapshot{false};std::atomic<uint32_t> discontinuities{0};
bool media=false,on=true,lock=false,contended=false,race=false;
bool active(){return on;}bool mediaBusy(){return media;}
int microphoneMutex=0,reads=0,copies=0;
int xSemaphoreTakeRecursive(int,int){if(contended)return 0;assert(!lock);lock=true;if(race)streamingEnabled=true;return pdTRUE;}
void xSemaphoreGiveRecursive(int){assert(lock);lock=false;}
bool startMicrophone(){assert(lock);return true;}
struct I2S{size_t readBytes(char* bytes,size_t n){assert(lock);assert(!streamingEnabled&&!media&&!otaBusySnapshot);++reads;auto* pcm=reinterpret_cast<int16_t*>(bytes);for(size_t i=0;i<n/2;++i)pcm[i]=i-400;return n;}} microphoneI2S;
void feed(const int16_t* pcm,size_t count){assert(count==800);for(size_t i=0;i<count;++i)assert(pcm[i]==int(i)-400);++copies;}
${idle}
void once(){try{idleTask(nullptr);}catch(Done&){}assert(!lock);}
int main(){once();assert(reads==1&&copies==1);
  streamingEnabled=true;once();streamingEnabled=false;media=true;once();media=false;otaBusySnapshot=true;once();otaBusySnapshot=false;
  on=false;once();on=true;contended=true;once();contended=false;race=true;once();
  assert(reads==1&&copies==1);puts("PASS idle listener never steals recording PCM");}
`;
  assert.match(nativeTest(fixture),/PASS idle listener never steals/);
});
