// Local English phoneme command recognition. AFE receives a copy; saved PCM is untouched.
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_mn_models.h>
#include <model_path.h>
#include <mbedtls/sha256.h>
namespace ChakshuVoice {
constexpr size_t MODEL_BYTES=2177224;
constexpr char MODEL_SHA256[]="9bb7348b31891a89eb494f5995970a7fc52b765759e4992d471ab2901bf9c47c";
enum Status : uint8_t { STARTING=0,LISTENING=1,MODEL_MISSING=2,NO_MEMORY=3,MODEL_ERROR=4,VOICE_DISABLED=5 };
std::atomic<uint8_t> status{MODEL_MISSING};
std::atomic<bool> enabled{true};
std::atomic<int> persistEnabled{-1};
std::atomic<uint32_t> discontinuities{0},leaseAt{0},leaseConnection{0};
struct Block { uint16_t count;int16_t samples[800]; };
struct PendingCommand { uint8_t action;uint32_t epoch,at; };
QueueHandle_t pcmQueue=nullptr,commandQueue=nullptr;
TaskHandle_t feedHandle=nullptr,detectHandle=nullptr,idleHandle=nullptr;
const esp_afe_sr_iface_t* afe=nullptr;esp_afe_sr_data_t* afeData=nullptr;
esp_mn_iface_t* mn=nullptr;model_iface_data_t* mnData=nullptr;
srmodel_list_t* models=nullptr;void* weights=nullptr;int16_t* afeInput=nullptr;int feedSize=0;
BLECharacteristic* events=nullptr;
portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t serial=0,lastAt=0;uint8_t lastCommand=0,lastResult=0;
Gate gate;
// Espressif MN5 phoneme alphabet. Two pronunciations cover Chakshu's first vowel.
// Each action needs a fresh Hi Chakshu.
const char* phrases[]={"hi pnKso","hi paKso","TdK FbTb","KLgK FbTb","STnRT VgDmb","STnP VgDmb","eDmb nN","eDmb eF"};
const int ids[]={WAKE,WAKE,PHOTO,PHOTO,VIDEO_START,VIDEO_STOP,AUDIO_ON,AUDIO_OFF};
esp_mn_phrase_t commands[8]{};esp_mn_node_t nodes[9]{};
bool active(){return status.load()==LISTENING&&enabled.load();}
void feed(const int16_t* samples,size_t count) {
  if(!active()||!pcmQueue)return;
  while(count){Block block;block.count=std::min(count,size_t(800));memcpy(block.samples,samples,block.count*2);
    if(xQueueSend(pcmQueue,&block,0)!=pdTRUE){++discontinuities;return;}samples+=block.count;count-=block.count;}
}
void feedTask(void*) {
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  Block block;size_t filled=0;uint32_t epoch=discontinuities.load();
  for(;;){
    if(xQueueReceive(pcmQueue,&block,pdMS_TO_TICKS(100))!=pdTRUE)continue;
    if(!active()||otaBusy()){filled=0;continue;}
    if(epoch!=discontinuities.load()){epoch=discontinuities.load();filled=0;xQueueReset(pcmQueue);continue;}
    size_t at=0;
    while(at<block.count){const size_t n=std::min(size_t(feedSize)-filled,size_t(block.count)-at);
      memcpy(afeInput+filled,block.samples+at,n*2);at+=n;filled+=n;
      if(filled==size_t(feedSize)){afe->feed(afeData,afeInput);filled=0;}}
  }
}
void detectTask(void*) {
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  uint32_t epoch=discontinuities.load();
  for(;;){
    afe_fetch_result_t* result=afe->fetch_with_delay(afeData,pdMS_TO_TICKS(100));
    if(!active()||otaBusy()||epoch!=discontinuities.load()){
      epoch=discontinuities.load();gate.reset();mn->clean(mnData);afe->reset_buffer(afeData);continue;
    }
    if(!result||result->ret_value!=ESP_OK||!result->data)continue;
    const auto detected=mn->detect(mnData,result->data);
    if(detected==ESP_MN_STATE_DETECTED){
      const auto* matches=mn->get_results(mnData);
      if(matches&&matches->num>0){const uint8_t action=gate.accept(matches->command_id[0],matches->prob[0],millis());
        if(action){PendingCommand command{action,epoch,millis()};xQueueSend(commandQueue,&command,0);}}
      mn->clean(mnData);
    }else if(detected==ESP_MN_STATE_TIMEOUT){gate.reset();mn->clean(mnData);}
  }
}
void idleTask(void*) {
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  int16_t samples[800];
  for(;;){
    // Streaming and SD consumers supply their own copies. Never compete for I2S.
    if(!active()||streamingEnabled.load()||mediaBusy()||otaBusy()||
       xSemaphoreTakeRecursive(microphoneMutex,0)!=pdTRUE){vTaskDelay(pdMS_TO_TICKS(20));continue;}
    if(active()&&!streamingEnabled.load()&&!mediaBusy()&&!otaBusy()&&startMicrophone()){
      const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(samples),sizeof(samples));
      if(n&&!(n&1))feed(samples,n/2);else ++discontinuities;
    }
    xSemaphoreGiveRecursive(microphoneMutex);vTaskDelay(1);
  }
}
void cleanup() {
  if(feedHandle){vTaskDelete(feedHandle);feedHandle=nullptr;}
  if(detectHandle){vTaskDelete(detectHandle);detectHandle=nullptr;}
  if(idleHandle){vTaskDelete(idleHandle);idleHandle=nullptr;}
  if(pcmQueue){vQueueDelete(pcmQueue);pcmQueue=nullptr;}
  if(commandQueue){vQueueDelete(commandQueue);commandQueue=nullptr;}
  if(mnData){mn->destroy(mnData);mnData=nullptr;}
  if(afeData){afe->destroy(afeData);afeData=nullptr;}
  // srmodel_load owns the model metadata; weights remain our PSRAM allocation.
  if(models){esp_srmodel_deinit(models);models=nullptr;}
  free(weights);weights=nullptr;free(afeInput);afeInput=nullptr;
}
void initialize() {
  Preferences settings;if(settings.begin("chakshu-voice",true)){enabled.store(settings.getBool("enabled",true));settings.end();}
  if(!ChakshuStorage::ready){status=MODEL_MISSING;return;}
  File file=SD.open("/synap/models/srmodels.bin",FILE_READ);
  if(!file){status=MODEL_MISSING;return;}
  if(file.size()!=MODEL_BYTES){file.close();status=MODEL_ERROR;return;}
  // Preserve headroom for BLE/recovery, camera and SD before invoking the model allocator.
  if(ESP.getFreePsram()<MODEL_BYTES+3u*1024u*1024u){file.close();status=NO_MEMORY;return;}
  weights=ps_malloc(MODEL_BYTES);
  if(!weights){file.close();status=NO_MEMORY;return;}
  const size_t read=file.read(static_cast<uint8_t*>(weights),MODEL_BYTES);file.close();
  uint8_t digest[32];char hex[65]{};
  if(read!=MODEL_BYTES||mbedtls_sha256(static_cast<const unsigned char*>(weights),MODEL_BYTES,digest,0)!=0){status=MODEL_ERROR;cleanup();return;}
  for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
  if(strcmp(hex,MODEL_SHA256)){status=MODEL_ERROR;cleanup();return;}
  // Only byte-for-byte pinned weights reach Espressif's unbounded model parser.
  models=srmodel_load(weights);
  if(!models){status=MODEL_ERROR;cleanup();return;}
  char* name=esp_srmodel_filter(models,"mn5q8","en");
  if(!models||!name||(mn=esp_mn_handle_from_name(name))==nullptr){status=MODEL_ERROR;cleanup();return;}
  mnData=mn->create(name,8000);
  if(!mnData){status=NO_MEMORY;cleanup();return;}
  for(size_t i=0;i<8;++i){commands[i].command_id=ids[i];commands[i].string=const_cast<char*>(phrases[i]);
    commands[i].phonemes=const_cast<char*>(phrases[i]);nodes[i+1].phrase=&commands[i];nodes[i].next=&nodes[i+1];}
  if(mn->set_speech_commands(mnData,&nodes[0])!=nullptr){status=MODEL_ERROR;cleanup();return;}
  mn->set_det_threshold(mnData,0.90f);
  afe_config_t* config=afe_config_init("M",models,AFE_TYPE_SR,AFE_MODE_LOW_COST);
  if(!config){status=NO_MEMORY;cleanup();return;}
  config->wakenet_init=false;config->aec_init=false;config->se_init=false;
  config->memory_alloc_mode=AFE_MEMORY_ALLOC_MORE_PSRAM;
  // MultiNet recognizes the activation phrase too, so no fixed WakeNet phrase is used.
  afe=esp_afe_handle_from_config(config);afeData=afe?afe->create_from_config(config):nullptr;afe_config_free(config);
  if(!afeData){status=NO_MEMORY;cleanup();return;}
  feedSize=afe->get_feed_chunksize(afeData);
  if(feedSize<=0||feedSize>2048||afe->get_feed_channel_num(afeData)!=1||afe->get_fetch_chunksize(afeData)!=mn->get_samp_chunksize(mnData)){
    status=MODEL_ERROR;cleanup();return;
  }
  afeInput=static_cast<int16_t*>(ps_malloc(feedSize*2));pcmQueue=xQueueCreate(8,sizeof(Block));commandQueue=xQueueCreate(4,sizeof(PendingCommand));
  if(!afeInput||!pcmQueue||!commandQueue||
     xTaskCreatePinnedToCore(feedTask,"voice-feed",4096,nullptr,2,&feedHandle,0)!=pdPASS||
     xTaskCreatePinnedToCore(detectTask,"voice-detect",8192,nullptr,1,&detectHandle,1)!=pdPASS||
     xTaskCreatePinnedToCore(idleTask,"voice-idle",4096,nullptr,1,&idleHandle,0)!=pdPASS){status=NO_MEMORY;cleanup();return;}
  status=enabled.load()?LISTENING:VOICE_DISABLED;
  xTaskNotifyGive(feedHandle);xTaskNotifyGive(detectHandle);xTaskNotifyGive(idleHandle);
  Serial.printf("[VOICE] Hi Chakshu ready=%u psram=%lu\n",unsigned(active()),(unsigned long)ESP.getFreePsram());
}
void encode(uint8_t* bytes){
  memset(bytes,0,20);bytes[0]=0xCD;bytes[1]=1;bytes[2]=status.load();bytes[3]=enabled.load()?1:0;
  portENTER_CRITICAL(&stateMux);put32le(bytes+4,serial);bytes[8]=lastCommand;bytes[9]=lastResult;put32le(bytes+10,lastAt);portEXIT_CRITICAL(&stateMux);
  put32le(bytes+14,discontinuities.load());bytes[18]=ChakshuTransfer::offline.load()?1:0;
}
void tick() {
  if(!otaBusy()) {const int pending=persistEnabled.exchange(-1);if(pending>=0){
    Preferences settings;if(settings.begin("chakshu-voice",false)){settings.putBool("enabled",pending==1);settings.end();}
  }}
  PendingCommand command;if(!commandQueue||xQueueReceive(commandQueue,&command,0)!=pdTRUE)return;
  if(!active()||otaBusy()||command.epoch!=discontinuities.load()||uint32_t(millis()-command.at)>2000u)return;
  const uint8_t action=command.action;
  const bool online=deviceConnected.load()&&leaseConnection.load()==connectionGeneration.load()&&
    leaseAt.load()!=0&&uint32_t(millis()-leaseAt.load())<6000u;
  uint8_t result=0;
  if(!online){
    using namespace ChakshuTransfer;
    if(action==3||action==5){
      // Stop also works if a connected page has been suspended and its lease expired.
      ++localEpoch; // Cancel only queued local starts; preserve PWA transfer requests.
      if(offline.load())stopRequested.store(true);
      if(streamingEnabled.load())stopStreaming();
    }
    else if(action==1||action==2||action==4){
      if(action==1&&offline.load())photoRequested.store(true);
      else {
        Request request{};request.local=true;request.localEpoch=localEpoch.load();request.operation=action==1?11:action==2?5:10;
        if(offline.load()&&offlineMode.load()==request.operation) { /* already in this mode */ }
        else if(!exitRemoteStandby())result=1;
        else {
          if(action!=1&&streamingEnabled.load())stopStreaming();
          if(!requests||xQueueSend(requests,&request,0)!=pdTRUE)result=1;
          else if(offline.load())stopRequested.store(true); // Save before worker starts the new mode.
        }
      }
    }
  }
  portENTER_CRITICAL(&stateMux);++serial;lastCommand=action;lastResult=online?2:result;lastAt=millis();portEXIT_CRITICAL(&stateMux);
  if(online&&events){uint8_t bytes[20];encode(bytes);events->setValue(bytes,20);events->notify();}
}
class Callbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c)override{uint8_t bytes[20];encode(bytes);c->setValue(bytes,20);}
  void onWrite(BLECharacteristic* c)override{
    const String value=c->getValue();if(value.length()!=3||uint8_t(value[0])!=0xCC||uint8_t(value[1])!=1)return;
    const uint8_t op=value[2];
    if(op==2){leaseConnection=connectionGeneration.load();leaseAt=millis();return;}
    if(op==3){leaseAt=0;return;}
    if(op>1)return;
    enabled=op==1;persistEnabled=op;++discontinuities;leaseAt=0;
    if(mnData)status=enabled?LISTENING:VOICE_DISABLED;
    // Persist in the control task; never write flash from a BLE callback.
  }
};
void ble(BLEService* service){
  auto* control=service->createCharacteristic("4fa12356-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_WRITE);
  control->setCallbacks(new Callbacks());
  events=service->createCharacteristic("4fa12357-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  events->addDescriptor(new BLE2902());
#endif
}
}
