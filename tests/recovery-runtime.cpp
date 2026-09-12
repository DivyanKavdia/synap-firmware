#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_BLUEDROID_ENABLED 1
#define USE_REAL_I2S_MIC 1
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4,portMAX_DELAY=-1;
using SemaphoreHandle_t=void*;
int mutexStorage=0;
void* xSemaphoreCreateMutex(){return &mutexStorage;}void xSemaphoreTake(void*,int){}void xSemaphoreGive(void*){}
size_t freeHeap=180000;bool psram=true;
void* heap_caps_malloc(size_t size,int caps){if((caps&MALLOC_CAP_SPIRAM)&&!psram)return nullptr;return malloc(size);}
size_t heap_caps_get_free_size(int){return freeHeap;}
struct AudioFrame{uint32_t generation;uint16_t sequence;int16_t samples[800];};
enum class DeviceState{STREAMING};enum class ErrorCode{NONE,TRANSPORT_CHANGED};
std::atomic<bool> streamingEnabled{false},deviceConnected{true};
std::atomic<uint32_t> streamGeneration{1},connectionGeneration{1},captureDrops{0};
std::atomic<uint8_t> chunksPerFrame{1};
bool sleepPending=false,busy=false,transport=true;
uint32_t now=100;
uint32_t millis(){return now;}
bool otaBusy(){return busy;}
bool configureTransportFromPeerMtu(){return transport;}
unsigned microphoneStops=0;
void stopMicrophone(){++microphoneStops;}
void requestStreamError(ErrorCode,uint32_t){assert(false);}
void setDeviceState(DeviceState,ErrorCode){}
void updateStatusCharacteristic(bool){}
struct Cccd{bool enabled=true;bool getNotifications(){return enabled;}} cccd;
auto* audioCccd=&cccd;
struct BLECharacteristic{std::vector<uint8_t> bytes;unsigned notifications=0;uint8_t* getData(){return bytes.data();}size_t getLength(){return bytes.size();}void setValue(const uint8_t* value,size_t n){bytes.assign(value,value+n);}void notify(){++notifications;}};
struct BLECharacteristicCallbacks{virtual ~BLECharacteristicCallbacks()=default;virtual void onRead(BLECharacteristic*){}virtual void onWrite(BLECharacteristic*){}};
// INSERT RECOVERY
void encodeImaAdpcm(const int16_t* samples,uint8_t* output){memset(output,uint8_t(samples[0]),404);}
uint16_t emitted=0;uint32_t lastPace=0;
bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t*,uint32_t pace){assert(generation==streamGeneration);emitted=sequence;lastPace=pace;return true;}
int main(){
  initializeRecovery();assert(recoveryRing.capacity==600);
  BLECharacteristic characteristic;recoveryCharacteristic=&characteristic;
  RecoveryCallbacks callbacks;auto* callback=static_cast<BLECharacteristicCallbacks*>(&callbacks);
  callback->onRead(&characteristic);assert(characteristic.bytes[2]==1 && characteristic.bytes.size()==16);
  auto request=[&](uint8_t command,uint8_t token,uint16_t seq){characteristic.bytes.assign(command==2?11:9,token);characteristic.bytes[0]=command;if(command==2){characteristic.bytes[9]=seq&255;characteristic.bytes[10]=seq>>8;}callback->onWrite(&characteristic);processRecoveryRequest();};
  request(1,7,0);assert(recoveryEnabled);
  streamingEnabled=true;
  for(unsigned i=0;i<620;i++){AudioFrame f{};f.generation=1;f.sequence=uint16_t(65500+i);retainRecoveryFrame(f);}
  assert(recoveryRing.count==600 && captureDrops==20);
  recoveryWaiting=true;recoveryWaitingAt=100;
  request(2,8,65530);assert(recoveryWaiting && recoveryRing.cursor==0);
  transport=false;request(2,7,65530);assert(recoveryWaiting);transport=true;
  cccd.enabled=false;request(2,7,65530);assert(recoveryWaiting);cccd.enabled=true;
  request(2,7,65530);assert(!recoveryWaiting && recoveryRing.cursor==11);
  assert(sendRecoveryFrame() && emitted==65531 && lastPace==30000);
  for(unsigned i=0;i<5;i++){assert(sendRecoveryFrame());}
  assert(emitted==0);
  finishBufferedRecording();assert(recoveryFinishing && microphoneStops==1 && (characteristic.bytes[2]&8));
  finishBufferedRecording();assert(microphoneStops==1);
  while(recoveryCanSend()){assert(sendRecoveryFrame());}
  assert(lastPace==45000 && !recoveryCanSend());
  resetRecovery(true);assert(!recoveryEnabled && !recoveryFinishing && !recoveryRing.count);
  request(1,9,0);assert(!recoveryEnabled);streamingEnabled=false;request(1,9,0);assert(recoveryEnabled);
  characteristic.bytes.assign(9,1);callback->onWrite(&characteristic);++connectionGeneration;processRecoveryRequest();assert(recoveryToken[0]==9);
  free(recoveryRing.frames);recoveryRing.frames=nullptr;recoveryRing.capacity=0;psram=false;
  initializeRecovery();assert(recoveryRing.capacity==100);
  free(recoveryRing.frames);recoveryRing.frames=nullptr;recoveryRing.capacity=0;freeHeap=120000;
  initializeRecovery();assert(!recoveryRing.frames && recoveryRing.capacity==0);
  std::cout<<"PASS recovery: wrap, bounded overflow, ownership, stale writes, transport, drain and low-memory fallback\n";
}
