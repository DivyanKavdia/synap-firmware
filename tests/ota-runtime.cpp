#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
// INSERT ENGINE
constexpr uint16_t SYNAP_FIRMWARE_BUILD=1200;
struct FakeFlash:Synap::OtaBackend {
  unsigned begins=0;
  bool matchesDevice(const uint8_t*)override{return true;}
  bool begin(uint32_t,const uint8_t*)override{++begins;return true;}
  bool write(const uint8_t*,size_t)override{return true;}
  Synap::OtaError finish()override{return Synap::OK;}
  bool commit()override{return true;}
  void abort()override{}
} backend;
Synap::OtaSession otaSession(backend);
std::atomic<bool> deviceConnected{true},streamingEnabled{true},otaOverflow{false},otaBusySnapshot{false};
std::atomic<uint32_t> connectionGeneration{1};
uint32_t clockNow=100;
bool critical=false;
enum class DeviceState{DISCONNECTED,CONNECTED_IDLE,STREAMING};
enum class ErrorCode{NONE};
DeviceState deviceState=DeviceState::STREAMING;
void setDeviceState(DeviceState state,ErrorCode){deviceState=state;}
void updateStatusLed(bool){}
uint32_t millis(){return clockNow;}
bool batteryCritical(){return critical;}
bool otaBusy(){return otaSession.busy();}
struct Characteristic {void setValue(const uint8_t*,size_t){} void notify(){}} status;
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
  m.data[0]=1;Synap::OtaSession::put32(m.data+1,7);Synap::OtaSession::put32(m.data+5,64);
  messages.push_back(m);
}
// INSERT PUBLISH
// INSERT TICK
int main(){
  // START can arrive before the first OTA capability refresh for the negotiated MTU.
  enqueueBegin();otaTick();
  assert(deviceState==DeviceState::STREAMING && streamingEnabled);
  assert(otaSession.error==Synap::BUSY && backend.begins==0 && !otaBusySnapshot);
  // Refresh from unsupported OTA MTU to supported OTA MTU while audio is active.
  server.mtu=32;otaTick();assert(deviceState==DeviceState::STREAMING);
  server.mtu=185;otaTick();assert(deviceState==DeviceState::STREAMING);
  streamingEnabled=false;deviceState=DeviceState::CONNECTED_IDLE;
  enqueueBegin();otaTick();assert(otaBusySnapshot && backend.begins==1);
  critical=true;otaTick();
  assert(!otaBusySnapshot && otaSession.state==Synap::FAILED && deviceState==DeviceState::CONNECTED_IDLE);
  assert(ESP.restarts==0);
  std::cout<<"PASS OTA refresh and refusal preserve recording; diagnostics snapshot follows OTA state\n";
}
