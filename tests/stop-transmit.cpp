#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#define USE_REAL_I2S_MIC 1
constexpr int pdTRUE=1,portMAX_DELAY=-1;
enum class DeviceState{DISCONNECTED,CONNECTED_IDLE,ERROR};
enum class ErrorCode{NONE,AUDIO_SOURCE_FAILED,TRANSPORT_CHANGED};
struct AudioFrame{uint32_t generation;uint16_t sequence;int16_t samples[800];};
std::atomic<bool> streamingEnabled{true},deviceConnected{true},transmitterActive{false};
std::atomic<uint32_t> streamGeneration{1};
std::atomic<bool> recoveryEnabled{false};
void resetRecovery(bool){}
int pdMS_TO_TICKS(int ms){return ms;}
bool recoveryCanSend(){return false;}
bool sendRecoveryFrame(){return false;}
int audioFrameQueue=1;
std::atomic<bool> acknowledged{false},stopWaiting{false},micStopped{false};
std::mutex gateMutex;
std::condition_variable gate;
bool sending=false,releaseSend=false;
unsigned received=0,errors=0;
struct Done{};
void vTaskDelay(int ticks){
  assert(ticks==1);stopWaiting=true;gate.notify_all();std::this_thread::yield();
}
void xQueueReset(int){}
void stopMicrophone(){micStopped=true;}
void applyCpuPowerProfile(bool active){assert(!active);}
void setDeviceState(DeviceState,ErrorCode){}
void updateStatusCharacteristic(bool notify){assert(notify && !transmitterActive);acknowledged=true;}
int xQueueReceive(int,AudioFrame* frame,int timeout){
  assert(timeout==portMAX_DELAY);
  if(received++)throw Done{};
  *frame={1,0,{}};return pdTRUE;
}
bool sendAudioFrame(const AudioFrame&){
  assert(transmitterActive && streamingEnabled);
  std::unique_lock<std::mutex> guard(gateMutex);sending=true;gate.notify_all();
  gate.wait(guard,[]{return releaseSend;});
  assert(!acknowledged);return true;
}
void requestStreamError(ErrorCode,uint32_t){++errors;}
// INSERT STOP
// INSERT TRANSMITTER
int main(){
  std::thread transmit([]{try{transmitterTask(nullptr);}catch(const Done&){};});
  {std::unique_lock<std::mutex> guard(gateMutex);gate.wait(guard,[]{return sending;});}
  std::thread stop([]{stopStreaming(ErrorCode::NONE);});
  {std::unique_lock<std::mutex> guard(gateMutex);
    gate.wait(guard,[]{return stopWaiting.load();});
    assert(!streamingEnabled && micStopped && !acknowledged);
    releaseSend=true;gate.notify_all();
  }
  transmit.join();stop.join();
  assert(acknowledged && !transmitterActive && errors==0);
  // A dequeued frame from before STOP must not send or leave the activity flag set.
  received=0;acknowledged=false;
  try{transmitterTask(nullptr);}catch(const Done&){}
  assert(!transmitterActive && errors==0);
  std::cout<<"PASS STOP waits for transmit completion and stale frames never send\n";
}
