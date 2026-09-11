#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#define USE_REAL_I2S_MIC 1
constexpr int pdTRUE=1;
constexpr uint32_t portMAX_DELAY=0xffffffffu;
struct AudioFrame {uint32_t generation;uint16_t sequence;int16_t samples[800];};
enum class ErrorCode {AUDIO_SOURCE_FAILED};
std::atomic<bool> streamingEnabled{false},deviceConnected{true};
std::atomic<uint32_t> streamGeneration{0},capturedFrames{0},captureDrops{0};
int audioFrameQueue=1;
int notifications=0,waits=0,captures=0,errors=0;
std::vector<AudioFrame> queued;
struct Done{};
void start(){++streamGeneration;streamingEnabled=true;++notifications;}
uint32_t ulTaskNotifyTake(int clear,uint32_t timeout){
  assert(clear==pdTRUE && timeout==portMAX_DELAY);++waits;
  // START immediately before blocking must remain pending until consumed.
  if(waits==1 || waits==3)start();
  if(waits==4)throw Done{};
  const auto result=notifications;notifications=0;return result;
}
bool acquireAudioFrame(AudioFrame&){
  ++captures;
  if(captures==1)return true;
  if(captures==2){streamingEnabled=false;return true;}
  if(captures==3)return false;
  return true;
}
int xQueueSend(int,const AudioFrame* frame,int){queued.push_back(*frame);return pdTRUE;}
void requestStreamError(ErrorCode error,uint32_t generation){
  assert(error==ErrorCode::AUDIO_SOURCE_FAILED && generation==2);++errors;streamingEnabled=false;
}
// INSERT ACQUISITION TASK
int main(){
  try{acquisitionTask(nullptr);}catch(const Done&){}
  assert(waits==4 && captures==3 && errors==1);
  assert(queued.size()==1 && queued[0].generation==1 && queued[0].sequence==0);
  assert(capturedFrames==1 && captureDrops==0);
  std::cout<<"PASS capture wake, stop cancellation and restart\n";
}
