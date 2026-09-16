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
enum class DeviceState{DISCONNECTED,CONNECTED_IDLE,STREAMING,ERROR};
enum class ErrorCode{NONE,TRANSPORT_CHANGED,PROTOCOL_MISMATCH,BAD_COMMAND,AUDIO_NOT_SUBSCRIBED,MTU_TOO_SMALL,AUDIO_SOURCE_FAILED};
constexpr uint8_t PROTOCOL_VERSION=2,CMD_STOP=0,CMD_START=1,CMD_GET_STATUS=2,CMD_STANDBY=3,CMD_WAKE=4,POWER_STATE_AWAKE=1;
std::atomic<bool> streamingEnabled{false},deviceConnected{true};
std::atomic<uint32_t> streamGeneration{1},connectionGeneration{1},captureDrops{0};
std::atomic<uint32_t> notifyRejected{0};
std::atomic<uint32_t> capturedFrames{0};
std::atomic<bool> transmitterActive{false};
int audioFrameQueue=1,captureTaskHandle=1;
void xQueueReset(int){}void xTaskNotifyGive(int){}
void applyCpuPowerProfile(bool){}
bool remoteStandby=false;
bool exitRemoteStandby(){remoteStandby=false;return true;}
void enterRemoteStandby(){remoteStandby=true;}
void publishPowerEvent(uint8_t){}void sampleBattery(bool){}
unsigned congestionWaits=0;
unsigned pdMS_TO_TICKS(unsigned ms){return ms;}
void vTaskDelay(unsigned ticks){assert(ticks==30);++congestionWaits;}
std::atomic<uint8_t> chunksPerFrame{1};
bool sleepPending=false,busy=false,transport=true;
uint32_t now=100;
uint32_t millis(){return now;}
bool otaBusy(){return busy;}
unsigned transportConfigurations=0;
bool configureTransportFromPeerMtu(){++transportConfigurations;return transport;}
unsigned microphoneStops=0;
void stopMicrophone(){++microphoneStops;}
bool startMicrophone(){return true;}
void stopStreaming(ErrorCode reason=ErrorCode::NONE);
void requestStreamError(ErrorCode,uint32_t){assert(false);}
void setDeviceState(DeviceState,ErrorCode){}
void updateStatusCharacteristic(bool){}
struct Cccd{bool enabled=true;bool getNotifications(){return enabled;}} cccd;
auto* audioCccd=&cccd;
struct BLECharacteristic{std::vector<uint8_t> bytes;unsigned notifications=0;uint8_t* getData(){return bytes.data();}size_t getLength(){return bytes.size();}void setValue(const uint8_t* value,size_t n){bytes.assign(value,value+n);}void notify(){++notifications;}};
struct BLECharacteristicCallbacks{virtual ~BLECharacteristicCallbacks()=default;virtual void onRead(BLECharacteristic*){}virtual void onWrite(BLECharacteristic*){}};
// INSERT RECOVERY
// INSERT RECORDING COMMANDS
void encodeImaAdpcm(const int16_t*,uint8_t*){assert(false && "Recovery must retain original PCM");}
uint16_t emitted=0;uint32_t lastPace=0;
bool rejectNotification=false;
bool reconnectDuringSend=false;
bool replayDuringSend=false;
bool sendCapturedFrame(const AudioFrame& frame,uint32_t pace){
  const auto sequence=frame.sequence;
  assert(frame.generation==streamGeneration);
  for(unsigned i=0;i<800;++i)assert(frame.samples[i]==int16_t((unsigned(sequence)+i)%65536-32768));
  if(reconnectDuringSend){++connectionGeneration;return false;}
  if(replayDuringSend){
    recoveryRequest.command=3;memset(recoveryRequest.token,7,8);
    recoveryRequest.sequence=65530;recoveryRequest.connection=connectionGeneration;
    processRecoveryRequest();replayDuringSend=false;
  }
  if(rejectNotification){++notifyRejected;return false;}
  emitted=sequence;lastPace=pace;return true;
}
int main(){
  initializeRecovery();assert(recoveryRing.capacity==600);
  BLECharacteristic characteristic;recoveryCharacteristic=&characteristic;
  RecoveryCallbacks callbacks;auto* callback=static_cast<BLECharacteristicCallbacks*>(&callbacks);
  callback->onRead(&characteristic);assert(characteristic.bytes[2]==0x11 && characteristic.bytes.size()==16);
  auto request=[&](uint8_t command,uint8_t token,uint16_t seq){characteristic.bytes.assign(command==1?9:11,token);characteristic.bytes[0]=command;if(command!=1){characteristic.bytes[9]=seq&255;characteristic.bytes[10]=seq>>8;}callback->onWrite(&characteristic);processRecoveryRequest();};
  request(1,7,0);assert(recoveryEnabled);
  streamingEnabled=true;
  for(unsigned i=0;i<620;i++){AudioFrame f{};f.generation=1;f.sequence=uint16_t(65500+i);for(unsigned j=0;j<800;++j)f.samples[j]=int16_t((unsigned(f.sequence)+j)%65536-32768);retainRecoveryFrame(f);}
  assert(recoveryRing.count==600 && captureDrops==20);
  recoveryWaiting=true;recoveryWaitingAt=100;
  request(3,7,65530);assert(recoveryWaiting && recoveryReplayAck==0);
  request(2,8,65530);assert(recoveryWaiting && recoveryRing.cursor==0);
  transport=false;request(2,7,65530);assert(recoveryWaiting);transport=true;
  cccd.enabled=false;request(2,7,65530);assert(recoveryWaiting);cccd.enabled=true;
  request(2,7,65530);assert(!recoveryWaiting && recoveryRing.cursor==11);
  rejectNotification=true;
  assert(!sendRecoveryFrame());assert(recoveryRing.cursor==11 && congestionWaits==1 && streamingEnabled);
  rejectNotification=false;
  // A delayed send on the replaced connection cannot error-stop the resumed take.
  reconnectDuringSend=true;
  assert(!sendRecoveryFrame());assert(recoveryRing.cursor==11 && streamingEnabled);
  reconnectDuringSend=false;
  assert(sendRecoveryFrame() && emitted==65531 && lastPace==30000);
  for(unsigned i=0;i<5;i++){assert(sendRecoveryFrame());}
  assert(emitted==0);
  const auto configured=transportConfigurations;
  request(3,8,65530);assert(recoveryReplayAck==0 && recoveryRing.cursor==17);
  cccd.enabled=false;request(3,7,65530);assert(recoveryReplayAck==0);cccd.enabled=true;
  // A live send finishing after the replay command cannot skip the first
  // requested frame, even when the two sequences cross uint16 wrap.
  replayDuringSend=true;assert(sendRecoveryFrame());
  assert(recoveryReplayAck==1 && recoveryRing.cursor==11);
  assert(sendRecoveryFrame() && emitted==65531);
  assert(transportConfigurations==configured && streamingEnabled && !recoveryWaiting);
  // If the requested boundary expired, replay starts at the oldest retained PCM.
  request(3,7,65500);assert(recoveryRing.cursor==0 && recoveryReplayAck==2);
  assert(sendRecoveryFrame() && emitted==65520);
  recoveryReplayAck=255;request(3,7,65530);assert(recoveryReplayAck==0);
  callback->onRead(&characteristic);assert(characteristic.bytes[3]==0);
  finishBufferedRecording();assert(recoveryFinishing && microphoneStops==1 && (characteristic.bytes[2]&8));
  const auto drainingCursor=recoveryRing.cursor;
  request(3,7,65500);assert(recoveryRing.cursor==drainingCursor && recoveryReplayAck==0);
  finishBufferedRecording();assert(microphoneStops==1);
  while(recoveryCanSend()){assert(sendRecoveryFrame());}
  assert(lastPace==45000 && !recoveryCanSend());
  resetRecovery(true);assert(!recoveryEnabled && !recoveryFinishing && !recoveryRing.count);
  request(1,9,0);assert(!recoveryEnabled);streamingEnabled=false;request(1,9,0);assert(recoveryEnabled);
  characteristic.bytes.assign(9,1);callback->onWrite(&characteristic);++connectionGeneration;processRecoveryRequest();assert(recoveryToken[0]==9);
  auto offlineStop=[&](bool expire){
    startStreaming(PROTOCOL_VERSION);
    AudioFrame frame{};frame.generation=streamGeneration;frame.sequence=0;
    for(unsigned i=0;i<800;i++)frame.samples[i]=int16_t(i-32768);
    retainRecoveryFrame(frame);
    deviceConnected=false;recoveryWaiting=true;
    const auto held=recoveryRing.count;
    const auto stops=microphoneStops;
    processCommand(CMD_STOP,PROTOCOL_VERSION);
    assert(streamingEnabled && recoveryFinishing && recoveryWaiting && recoveryRing.count==held);
    assert(microphoneStops==stops+1);
    callback->onRead(&characteristic);
    assert((characteristic.bytes[2]&0x20) && (characteristic.bytes[2]&8));
    const auto ownerHash=std::vector<uint8_t>(characteristic.bytes.begin()+12,characteristic.bytes.end());
    if(!expire){
      deviceConnected=true;request(2,9,0xffff);
      assert(!recoveryWaiting && recoveryFinishing && streamingEnabled);
      assert(sendRecoveryFrame() && emitted==0 && !recoveryCanSend());
      assert(microphoneStops==stops+1);
      stopStreaming();request(1,9,0);
      return;
    }
    // Even after the bounded drain expires, the same owner must learn Stop.
    stopStreaming(ErrorCode::TRANSPORT_CHANGED);
    callback->onRead(&characteristic);
    assert(!streamingEnabled && !recoveryFinishing && !recoveryWaiting && !recoveryEnabled && !recoveryRing.count);
    assert((characteristic.bytes[2]&0x20) && std::vector<uint8_t>(characteristic.bytes.begin()+12,characteristic.bytes.end())==ownerHash);
    deviceConnected=true;
    stopStreaming(); // Reconciliation may stop the idle transport again.
    callback->onRead(&characteristic);assert(characteristic.bytes[2]&0x20);
    request(1,9,0);callback->onRead(&characteristic);
    assert(recoveryEnabled && !(characteristic.bytes[2]&0x20));
    startStreaming(PROTOCOL_VERSION);processCommand(CMD_STOP,PROTOCOL_VERSION);
    assert(recoveryFinishing);
    stopStreaming();startStreaming(PROTOCOL_VERSION);
    callback->onRead(&characteristic);assert(!(characteristic.bytes[2]&0x20));
    stopStreaming();
  };
  offlineStop(false);offlineStop(true);
  free(recoveryRing.frames);recoveryRing.frames=nullptr;recoveryRing.capacity=0;psram=false;
  initializeRecovery();assert(recoveryRing.capacity==25);
  request(1,9,0);offlineStop(false);offlineStop(true);
  free(recoveryRing.frames);recoveryRing.frames=nullptr;recoveryRing.capacity=0;freeHeap=120000;
  initializeRecovery();assert(!recoveryRing.frames && recoveryRing.capacity==0);
  std::cout<<"PASS recovery: wrap, bounded overflow, ownership, stale writes, transport, drain and low-memory fallback\n";
}
