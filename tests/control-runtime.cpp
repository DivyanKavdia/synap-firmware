#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#define USE_REAL_I2S_MIC 1
#define CONFIG_BLUEDROID_ENABLED 1
constexpr int pdTRUE=1;
constexpr uint8_t PROTOCOL_VERSION=2,CMD_START=1,AUDIO_HEADER_BYTES=8;
constexpr uint8_t POWER_STATE_AWAKE=1,POWER_STATE_STANDBY=2;
// INSERT CONTROL TYPES
std::atomic<bool> deviceConnected{false},streamingEnabled{false},connectionEventPending{false};
std::atomic<uint32_t> connectionGeneration{0},streamGeneration{0};
std::atomic<uint32_t> capturedFrames{0},captureDrops{0},notifyRejected{0};
std::atomic<uint16_t> peerMtu{23},attValueCapacity{20},audioPayloadBytes{0};
std::atomic<uint8_t> chunksPerFrame{0};
DeviceState deviceState=DeviceState::DISCONNECTED;
ErrorCode lastError=ErrorCode::NONE;
bool remoteStandby=false,restartAdvertising=false,transportValid=true,microphoneValid=true,busy=false;
uint32_t disconnectedAt=0,clockNow=100;
unsigned stops=0,powerEvents=0,batterySamples=0,starts=0,cpuActive=0,wakes=0,commands=0,maintenance=0,loops=0;
uint8_t lastPower=0;
int controlQueue=1,audioFrameQueue=2,captureTaskHandle=3;
struct Cccd {bool subscribed=true;bool getNotifications(){return subscribed;}} cccd;
auto* audioCccd=&cccd;
struct BLEServer {
  unsigned advertisements=0;
  uint16_t getPeerMTU(int){return 23;}
  int getConnId(){return 0;}
  void startAdvertising(){++advertisements;}
} server;
auto* bleServer=&server;
struct BLEServerCallbacks {
  virtual ~BLEServerCallbacks()=default;
  virtual void onConnect(BLEServer*){}
  virtual void onDisconnect(BLEServer*){}
};
uint32_t millis(){return clockNow;}
int pdMS_TO_TICKS(int ms){return ms;}
bool otaBusy(){return busy;}
void updateStatusCharacteristic(bool){}
void setDeviceState(DeviceState state,ErrorCode error){deviceState=state;lastError=error;}
void stopStreaming(ErrorCode reason=ErrorCode::NONE){
  ++stops;streamingEnabled=false;++streamGeneration;lastError=reason;
  deviceState=deviceConnected?(reason==ErrorCode::NONE?DeviceState::CONNECTED_IDLE:DeviceState::ERROR):DeviceState::DISCONNECTED;
}
void publishPowerEvent(uint8_t state){++powerEvents;lastPower=state;}
void sampleBattery(bool){++batterySamples;}
void applyCpuPowerProfile(bool active){if(active)++cpuActive;}
bool configureTransportFromPeerMtu(){return transportValid;}
bool startMicrophone(){++starts;return microphoneValid;}
void xQueueReset(int){}
void xTaskNotifyGive(int){++wakes;}
// INSERT START
// INSERT CALLBACKS
ServerCallbacks callbacks;
auto* link=static_cast<BLEServerCallbacks*>(&callbacks);
void processCommand(uint8_t command,uint8_t version){
  assert(command==CMD_START && version==PROTOCOL_VERSION);
  assert(deviceState==DeviceState::CONNECTED_IDLE);++commands;startStreaming(version);
}
void pollTouchControl(){}
void otaTick(){++maintenance;}
void powerTick(){}
struct Done{};
void updateStatusLed(){if(loops==4)throw Done{};}
int xQueueReceive(int,ControlMessage* message,int timeout){
  assert(timeout==10);++loops;clockNow=501;
  *message={EventType::COMMAND,CMD_START,PROTOCOL_VERSION,0,0};
  if(loops==2){link->onConnect(&server);message->connection=connectionGeneration;}
  if(loops==3){
    message->connection=connectionGeneration;
    link->onDisconnect(&server);link->onConnect(&server);
  }
  return loops==4?0:pdTRUE;
}
// INSERT CONTROL TASK
int main(){
  // Neither callback depends on queue capacity; no queue sender is supplied here.
  link->onConnect(&server);assert(connectionEventPending && deviceConnected);
  reconcileConnection();assert(deviceState==DeviceState::CONNECTED_IDLE && stops==1);
  assert(!connectionEventPending && !restartAdvertising && disconnectedAt==0);
  remoteStandby=true;
  link->onDisconnect(&server);link->onConnect(&server);
  reconcileConnection();assert(stops==2 && lastPower==POWER_STATE_STANDBY);
  link->onDisconnect(&server);assert(!streamingEnabled && !deviceConnected);
  reconcileConnection();assert(restartAdvertising && disconnectedAt==100 && deviceState==DeviceState::DISCONNECTED);
  remoteStandby=false;
  try{controlTask(nullptr);}catch(const Done&){}
  assert(commands==1 && maintenance==4 && server.advertisements==1);
  assert(deviceState==DeviceState::CONNECTED_IDLE && !connectionEventPending);

  transportValid=false;starts=0;cpuActive=0;wakes=0;
  startStreaming(PROTOCOL_VERSION);
  assert(lastError==ErrorCode::MTU_TOO_SMALL && starts==0 && cpuActive==0 && wakes==0);
  transportValid=true;microphoneValid=false;
  startStreaming(PROTOCOL_VERSION);assert(lastError==ErrorCode::AUDIO_SOURCE_FAILED && !streamingEnabled);
  microphoneValid=true;starts=0;wakes=0;
  startStreaming(PROTOCOL_VERSION);
  const uint32_t generation=streamGeneration;
  assert(streamingEnabled && starts==1 && wakes==1);
  startStreaming(PROTOCOL_VERSION);
  assert(streamGeneration==generation && starts==1 && wakes==1);
  std::cout<<"PASS link recovery, stale commands, standby, advertising and START admission\n";
}
