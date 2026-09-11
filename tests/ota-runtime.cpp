#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
// INSERT ENGINE
constexpr uint16_t SYNAP_FIRMWARE_BUILD=1200;
bool cpuActive=false;
void applyCpuPowerProfile(bool active){cpuActive=active;}
struct FakeFlash:Synap::OtaBackend {
  unsigned begins=0,writes=0;
  bool matchesDevice(const uint8_t*)override{return true;}
  bool begin(uint32_t,const uint8_t*)override{assert(cpuActive);++begins;return true;}
  bool write(const uint8_t*,size_t)override{assert(cpuActive);++writes;return true;}
  Synap::OtaError finish()override{assert(cpuActive);return Synap::OK;}
  bool commit()override{assert(cpuActive);return true;}
  void abort()override{}
} backend;
Synap::OtaSession otaSession(backend);
std::atomic<bool> deviceConnected{true},streamingEnabled{true},otaOverflow{false},otaBusySnapshot{false};
std::atomic<uint32_t> connectionGeneration{1};
uint32_t clockNow=100,otaLastActivityAt=0;
bool critical=false;
enum class DeviceState{DISCONNECTED,CONNECTED_IDLE,STREAMING};
enum class ErrorCode{NONE};
DeviceState deviceState=DeviceState::STREAMING;
void setDeviceState(DeviceState state,ErrorCode){deviceState=state;}
void updateStatusLed(bool){}
uint32_t millis(){return clockNow;}
bool batteryCritical(){return critical;}
bool otaBusy(){return otaSession.busy();}
// INSERT POWER
struct Characteristic {
  std::vector<uint8_t> value;
  std::vector<std::vector<uint8_t>> notifications;
  void setValue(const uint8_t* data,size_t size){value.assign(data,data+size);}
  void notify(){notifications.push_back(value);}
} status;
auto* otaStatusCharacteristic=&status;
struct Server {uint16_t mtu=185;uint16_t getPeerMTU(int){return mtu;} int getConnId(){return 0;}} server;
auto* bleServer=&server;
struct Esp {unsigned restarts=0;void restart(){++restarts;}} ESP;
struct esp_partition_t {uint32_t address;uint32_t size;};
esp_partition_t running{0x10000,0x140000},target{0x150000,0x140000};
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_OTA=1,ESP_PARTITION_TYPE_APP=0;
constexpr int ESP_PARTITION_SUBTYPE_APP_OTA_0=0,ESP_PARTITION_SUBTYPE_APP_OTA_1=1,pdTRUE=1;
const esp_partition_t* esp_ota_get_next_update_partition(void*){return &target;}
const esp_partition_t* esp_partition_find_first(int,int,const char*){return &target;}
const esp_partition_t* esp_ota_get_running_partition(){return &running;}
struct OtaMessage {uint32_t connection;uint16_t length;uint8_t data[512];};
std::vector<OtaMessage> messages;
int otaQueue=1;
int xQueueReceive(int,OtaMessage* message,int){
  if(messages.empty())return 0;
  *message=messages.front();messages.erase(messages.begin());return pdTRUE;
}
void enqueueBegin(){
  OtaMessage m{};m.connection=connectionGeneration;m.length=59;
  m.data[0]=1;put32le(m.data+1,7);put32le(m.data+5,64);
  messages.push_back(m);
}
void enqueueCommand(uint8_t command){
  OtaMessage m{};m.connection=connectionGeneration;m.length=5;
  m.data[0]=command;put32le(m.data+1,7);messages.push_back(m);
}
// INSERT PUBLISH
// INSERT TICK
void tick(){otaTick();applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());}
int main(){
  // START can arrive before the first OTA capability refresh for the negotiated MTU.
  enqueueBegin();tick();
  assert(deviceState==DeviceState::STREAMING && streamingEnabled);
  assert(otaSession.error==Synap::BUSY && backend.begins==0 && !otaBusySnapshot);
  assert(status.notifications.size()==1 && status.value[3]==Synap::BUSY);
  tick();assert(status.notifications.size()==1); // Unchanged idle ticks publish nothing.
  // Refresh from unsupported OTA MTU to supported OTA MTU while audio is active.
  server.mtu=32;tick();assert(deviceState==DeviceState::STREAMING);
  server.mtu=185;tick();assert(deviceState==DeviceState::STREAMING);
  assert(status.notifications.size()==3);
  streamingEnabled=false;deviceState=DeviceState::CONNECTED_IDLE;
  enqueueBegin();tick();assert(otaBusySnapshot && backend.begins==1);
  assert(status.notifications.size()==4 && cpuActive);
  clockNow+=999;tick();assert(cpuActive);
  ++clockNow;tick();assert(!cpuActive && otaBusySnapshot);
  assert(otaSession.state==Synap::RECEIVING && otaSession.offset==0);
  critical=true;tick();
  assert(!otaBusySnapshot && otaSession.state==Synap::FAILED && deviceState==DeviceState::CONNECTED_IDLE);
  assert(status.notifications.size()==5 && status.value[2]==Synap::FAILED);
  critical=false;
  enqueueBegin();tick();
  otaOverflow=true;tick();
  assert(status.notifications.size()==7 && otaSession.error==Synap::BAD_PACKET && !otaBusySnapshot);
  enqueueBegin();tick();
  clockNow+=900001;tick();
  assert(status.notifications.size()==9 && otaSession.error==Synap::TIMED_OUT);

  // Burst ACKs remain ordered, and repeated DATA is acknowledged without another flash write.
  enqueueBegin();
  OtaMessage data{};data.connection=connectionGeneration;data.length=73;
  data.data[0]=2;put32le(data.data+1,7);data.data[9]=0xe9;data.data[21]=9;
  put32le(data.data+41,0xabcd5432);
  messages.push_back(data);messages.push_back(data);enqueueCommand(3);
  tick();
  assert(status.notifications.size()==13 && backend.writes==1 && otaSession.state==Synap::READY);
  for(size_t i=10;i<=12;++i)assert(Synap::OtaSession::u32(status.notifications[i].data()+8)==64);
  deviceConnected=false;tick();assert(!cpuActive && otaBusySnapshot);
  ++connectionGeneration;deviceConnected=true;
  enqueueBegin();messages.back().data[0]=6;tick();assert(cpuActive);
  assert(status.notifications.size()==14 && otaSession.state==Synap::READY && otaBusySnapshot);
  clockNow+=1000;tick();assert(!cpuActive);
  enqueueCommand(4);tick();
  assert(status.notifications.size()==15 && otaSession.state==Synap::COMMITTED);
  assert(ESP.restarts==0);
  clockNow+=1501;tick();assert(ESP.restarts==1 && status.notifications.size()==15);
  otaSession.state=Synap::RECEIVING;otaLastActivityAt=0xFFFFFFF0u;
  clockNow=0x10u;assert(otaNeedsActiveCpu());
  clockNow=984u;assert(!otaNeedsActiveCpu());
  std::cout<<"PASS OTA refresh and refusal preserve recording; command/retry ACKs survive status coalescing\n";
}
