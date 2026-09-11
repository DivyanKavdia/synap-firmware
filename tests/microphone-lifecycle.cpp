#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#define USE_REAL_I2S_MIC 1
constexpr uint32_t SAMPLE_RATE=16000,portMAX_DELAY=0xffffffffu;
constexpr uint16_t SAMPLES_PER_FRAME=800;
constexpr int I2S_BCLK_PIN=4,I2S_WS_PIN=5,I2S_DATA_IN_PIN=6;
constexpr int I2S_MODE_STD=1,I2S_DATA_BIT_WIDTH_32BIT=32,I2S_SLOT_MODE_MONO=1,I2S_STD_SLOT_LEFT=0;
struct AudioFrame {uint32_t generation;uint16_t sequence;int16_t samples[800];};
std::atomic<bool> streamingEnabled{true},microphoneValidated{false};
std::atomic<uint32_t> streamGeneration{1};
bool microphoneReady=false;
std::recursive_mutex driverMutex;
auto* microphoneMutex=&driverMutex;
std::mutex gateMutex;
std::condition_variable gate;
bool lockContended=false,readActive=false,beginActive=false,releaseDriver=false;
bool holdRead=false,holdBegin=false;
unsigned endCalls=0,emptyReads=0;
int xSemaphoreTakeRecursive(std::recursive_mutex* mutex,uint32_t timeout){
  assert(timeout==portMAX_DELAY);
  if(!mutex->try_lock()){
    {std::lock_guard<std::mutex> guard(gateMutex);lockContended=true;gate.notify_all();}
    mutex->lock();
  }
  return 1;
}
void xSemaphoreGiveRecursive(std::recursive_mutex* mutex){mutex->unlock();}
struct SerialStub {void println(const char*){} template<class... T> void printf(const char*,T...){} } Serial;
int pdMS_TO_TICKS(int ms){return ms;}
void vTaskDelay(int){}
struct FakeI2S {
  void setPins(int,int,int,int){}
  void setTimeout(int timeout){assert(timeout==80);}
  bool begin(int,uint32_t,int,int,int){
    std::unique_lock<std::mutex> guard(gateMutex);
    assert(!readActive);beginActive=true;gate.notify_all();
    if(holdBegin)gate.wait(guard,[]{return releaseDriver;});
    beginActive=false;return true;
  }
  void end(){
    std::lock_guard<std::mutex> guard(gateMutex);
    assert(!readActive && !beginActive);++endCalls;
  }
  size_t readBytes(char* destination,size_t size){
    std::unique_lock<std::mutex> guard(gateMutex);
    assert(!beginActive);readActive=true;gate.notify_all();
    if(holdRead)gate.wait(guard,[]{return releaseDriver;});
    readActive=false;
    if(emptyReads){--emptyReads;return 0;}
    memset(destination,0,size);return size;
  }
} microphoneI2S;
// INSERT GUARD
// INSERT MICROPHONE FUNCTIONS
// INSERT CAPTURE
int main(){
  for(bool recovery: {false,true}){
    streamingEnabled=true;streamGeneration=recovery?3:1;
    {std::lock_guard<std::mutex> guard(gateMutex);
      holdRead=false;holdBegin=false;releaseDriver=false;lockContended=false;endCalls=0;emptyReads=0;
    }
    assert(startMicrophone());assert(microphoneValidated.load());
    {std::lock_guard<std::mutex> guard(gateMutex);
      holdRead=!recovery;holdBegin=recovery;emptyReads=recovery?3:0;
    }
    AudioFrame frame{streamGeneration.load(),0,{}};
    std::thread capture([&]{acquireAudioFrame(frame);});
    {std::unique_lock<std::mutex> guard(gateMutex);
      gate.wait(guard,[&]{return recovery?beginActive:readActive;});
    }
    streamingEnabled=false;++streamGeneration;
    std::thread stop([]{stopMicrophone();});
    {std::unique_lock<std::mutex> guard(gateMutex);
      gate.wait(guard,[]{return lockContended;});
      assert(endCalls==(recovery?1u:0u));
      releaseDriver=true;gate.notify_all();
    }
    capture.join();stop.join();
    assert(!microphoneReady && endCalls==(recovery?2u:1u));
    assert(driverMutex.try_lock());driverMutex.unlock();
  }
  std::cout<<"PASS microphone shutdown serializes with reads and recovery initialization\n";
}
