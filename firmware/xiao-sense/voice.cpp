// Supported local voice flow: WakeNet "Hi ESP" gates a small MultiNet command window.
// Voice remains optional and starts only after BLE/runtime readiness.
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_mn_models.h>
#include <esp_wn_models.h>
#include <model_path.h>
#include <mbedtls/sha256.h>
namespace ChakshuVoice {
using ChakshuModel::MODEL_BYTES;
using ChakshuModel::MODEL_SHA256;
enum Status : uint8_t { STARTING=0,LISTENING=1,MODEL_MISSING=2,NO_MEMORY=3,MODEL_ERROR=4,VOICE_DISABLED=5 };
std::atomic<uint8_t> status{MODEL_MISSING};
std::atomic<bool> enabled{true};
std::atomic<int> persistEnabled{-1};
std::atomic<uint32_t> discontinuities{0},leaseAt{0},leaseConnection{0};
struct Block { uint16_t count;int16_t samples[800]; };
struct PendingCommand { uint8_t command;uint32_t epoch,at; };
QueueHandle_t pcmQueue=nullptr,commandQueue=nullptr;
TaskHandle_t feedHandle=nullptr,detectHandle=nullptr,idleHandle=nullptr,feedbackHandle=nullptr,initHandle=nullptr;
const esp_afe_sr_iface_t* afe=nullptr;esp_afe_sr_data_t* afeData=nullptr;
esp_mn_iface_t* mn=nullptr;model_iface_data_t* mnData=nullptr;char* wakeModel=nullptr;
srmodel_list_t* models=nullptr;srmodel_list_t* wakeModels=nullptr;void* weights=nullptr;int16_t* afeInput=nullptr;int feedSize=0;
BLECharacteristic* events=nullptr;
portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t serial=0,lastAt=0;uint8_t lastCommand=0,lastResult=0;uint16_t lastValue=0;
Gate gate;
// On XIAO ESP32S3 Sense GPIO21 is both the active-low USER LED and SD CS.
// Wake feedback must therefore own the media gate and return the pin HIGH.
constexpr uint8_t WAKE_LED_PIN=21;
constexpr uint16_t WAKE_LED_MS=90;
constexpr size_t MAX_PHRASES=12;
esp_mn_phrase_t commands[MAX_PHRASES]{};esp_mn_node_t nodes[MAX_PHRASES+1]{};
char phraseStorage[MAX_PHRASES][80]{};size_t phraseCount=0;

bool active(){return status.load()==LISTENING&&enabled.load()&&ChakshuFlashModel::present();}

bool addPhrase(uint8_t id,const String& phoneme) {
  if(!id||!phoneme.length()||phraseCount>=MAX_PHRASES)return false;
  snprintf(phraseStorage[phraseCount],sizeof(phraseStorage[phraseCount]),"%s",phoneme.c_str());
  commands[phraseCount].command_id=id;
  commands[phraseCount].string=phraseStorage[phraseCount];
  commands[phraseCount].phonemes=phraseStorage[phraseCount];
  nodes[phraseCount+1].phrase=&commands[phraseCount];
  nodes[phraseCount].next=&nodes[phraseCount+1];
  ++phraseCount;return true;
}
bool configurePhrases() {
  phraseCount=0;memset(commands,0,sizeof(commands));memset(nodes,0,sizeof(nodes));memset(phraseStorage,0,sizeof(phraseStorage));
  // MultiNet is only active after WakeNet. It never owns the wake phrase.
  return addPhrase(PHOTO,"TdK c SNaP") &&
    addPhrase(PHOTO,"TdK c FoTo") &&
    addPhrase(VIDEO_START,"RcKeRD c VgDmb") &&
    addPhrase(VIDEO_STOP,"STnP VgDmb") &&
    addPhrase(AUDIO_ON,"RcKeRD eDmb") &&
    addPhrase(AUDIO_OFF,"STnP eDmb") &&
    addPhrase(STOP,"STnP") &&
    addPhrase(DESCRIBE,"WcT Do Yo Sm");
}

void feed(const int16_t* samples,size_t count) {
  if(!active()||!pcmQueue||!samples)return;
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
void returnToWake(bool& commandWindow) {
  commandWindow=false;gate.reset();
  if(mnData)mn->clean(mnData);
  if(afeData)afe->enable_wakenet(afeData);
}
void detectTask(void*) {
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  uint32_t epoch=discontinuities.load();bool commandWindow=false;
  for(;;){
    afe_fetch_result_t* result=afe->fetch_with_delay(afeData,pdMS_TO_TICKS(100));
    if(!active()||otaBusy()||epoch!=discontinuities.load()){
      epoch=discontinuities.load();returnToWake(commandWindow);afe->reset_buffer(afeData);continue;
    }
    if(!result||result->ret_value!=ESP_OK)continue;
    const uint32_t now=millis();
    if(result->wakeup_state==WAKENET_DETECTED){
      gate.accept(WAKE,1.0f,now);commandWindow=true;mn->clean(mnData);
      afe->disable_wakenet(afeData);
      PendingCommand pending{WAKE,epoch,now};
      if(xQueueSend(commandQueue,&pending,0)!=pdTRUE)++discontinuities;
      continue;
    }
    if(!commandWindow||!result->data)continue;
    const auto detected=mn->detect(mnData,result->data);
    if(detected==ESP_MN_STATE_DETECTED){
      const auto* matches=mn->get_results(mnData);
      uint8_t command=0;
      if(matches&&matches->num>0)command=gate.accept(matches->command_id[0],matches->prob[0],millis());
      mn->clean(mnData);
      if(command){
        PendingCommand pending{command,epoch,millis()};
        if(xQueueSend(commandQueue,&pending,0)!=pdTRUE)++discontinuities;
        returnToWake(commandWindow);
      }
    }else if(detected==ESP_MN_STATE_TIMEOUT){
      returnToWake(commandWindow);
    }
  }
}
bool pulseWakeLed() {
  if(!active()||otaBusy())return false;
  ChakshuResources::Lease admission;
  if(!admission)return false; // Never toggle shared SD CS during recording/transfer.
  pinMode(WAKE_LED_PIN,OUTPUT);
  digitalWrite(WAKE_LED_PIN,LOW);
  vTaskDelay(pdMS_TO_TICKS(WAKE_LED_MS));
  digitalWrite(WAKE_LED_PIN,HIGH);
  return true;
}
void feedbackTask(void*) {
  for(;;){ulTaskNotifyTake(pdTRUE,portMAX_DELAY);pulseWakeLed();}
}
void idleTask(void*) {
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  int16_t samples[800];
  for(;;){
    // Streaming and SD recording paths feed their own copies. Idle listening is
    // the only task allowed to read PDM when neither consumer owns the microphone.
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
  if(feedbackHandle){vTaskDelete(feedbackHandle);feedbackHandle=nullptr;}
  if(pcmQueue){vQueueDelete(pcmQueue);pcmQueue=nullptr;}
  if(commandQueue){vQueueDelete(commandQueue);commandQueue=nullptr;}
  if(mnData){mn->destroy(mnData);mnData=nullptr;}
  if(afeData){afe->destroy(afeData);afeData=nullptr;}
  if(models){esp_srmodel_deinit(models);models=nullptr;}
  if(wakeModels){esp_srmodel_deinit(wakeModels);wakeModels=nullptr;}
  free(weights);weights=nullptr;free(afeInput);afeInput=nullptr;
}
void initialize() {
  status=STARTING;
  Preferences settings;if(settings.begin("chakshu-voice",true)){enabled.store(settings.getBool("enabled",true));settings.end();}
  if(!ChakshuFlashModel::present()){status=MODEL_MISSING;return;}
  // Voice is optional. Never consume the PSRAM reserve needed by core camera,
  // BLE recovery and SD transfer if the device cannot accommodate both.
  if(ESP.getFreePsram()<MODEL_BYTES+2u*1024u*1024u){status=NO_MEMORY;return;}
  weights=ps_malloc(MODEL_BYTES);
  if(!weights){status=NO_MEMORY;return;}
  const auto loaded=ChakshuFlashModel::load(static_cast<uint8_t*>(weights),MODEL_BYTES);
  if(loaded!=ChakshuFlashModel::LOADED){status=loaded==ChakshuFlashModel::NO_MEMORY?NO_MEMORY:MODEL_ERROR;cleanup();return;}
  uint8_t digest[32];char hex[65]{};
  if(mbedtls_sha256(static_cast<const unsigned char*>(weights),MODEL_BYTES,digest,0)!=0){status=MODEL_ERROR;cleanup();return;}
  for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
  if(strcmp(hex,MODEL_SHA256)){status=MODEL_ERROR;cleanup();return;}
  models=srmodel_load(weights);
  if(!models){status=MODEL_ERROR;cleanup();return;}
  wakeModels=srmodel_load(nullptr);
  wakeModel=wakeModels?esp_srmodel_filter(wakeModels,ESP_WN_PREFIX,"hiesp"):nullptr;
  char* name=esp_srmodel_filter(models,"mn5q8","en");
  if(!wakeModel||!name||(mn=esp_mn_handle_from_name(name))==nullptr){status=MODEL_ERROR;cleanup();return;}
  mnData=mn->create(name,5000);
  if(!mnData||!configurePhrases()){status=NO_MEMORY;cleanup();return;}
  if(mn->set_speech_commands(mnData,&nodes[0])!=nullptr){status=MODEL_ERROR;cleanup();return;}
  mn->set_det_threshold(mnData,0.82f);
  afe_config_t* config=afe_config_init("M",models,AFE_TYPE_SR,AFE_MODE_LOW_COST);
  if(!config){status=NO_MEMORY;cleanup();return;}
  config->wakenet_init=true;config->wakenet_model_name=wakeModel;config->wakenet_mode=DET_MODE_90;
  config->aec_init=false;config->se_init=false;config->ns_init=false;config->vad_init=false;
  config->memory_alloc_mode=AFE_MEMORY_ALLOC_MORE_PSRAM;
  afe=esp_afe_handle_from_config(config);afeData=afe?afe->create_from_config(config):nullptr;afe_config_free(config);
  if(!afeData){status=NO_MEMORY;cleanup();return;}
  if(afe->reset_wakenet_threshold(afeData,1)!=1){status=MODEL_ERROR;cleanup();return;}
  feedSize=afe->get_feed_chunksize(afeData);
  if(feedSize<=0||feedSize>2048||afe->get_feed_channel_num(afeData)!=1||afe->get_fetch_chunksize(afeData)!=mn->get_samp_chunksize(mnData)){
    status=MODEL_ERROR;cleanup();return;
  }
  afeInput=static_cast<int16_t*>(ps_malloc(feedSize*2));pcmQueue=xQueueCreate(8,sizeof(Block));commandQueue=xQueueCreate(4,sizeof(PendingCommand));
  if(!afeInput||!pcmQueue||!commandQueue||
     xTaskCreatePinnedToCore(feedTask,"voice-feed",4096,nullptr,2,&feedHandle,0)!=pdPASS||
     xTaskCreatePinnedToCore(detectTask,"voice-detect",8192,nullptr,1,&detectHandle,1)!=pdPASS||
     xTaskCreatePinnedToCore(idleTask,"voice-idle",4096,nullptr,1,&idleHandle,0)!=pdPASS||
     xTaskCreatePinnedToCore(feedbackTask,"voice-led",2048,nullptr,1,&feedbackHandle,0)!=pdPASS){status=NO_MEMORY;cleanup();return;}
  status=enabled.load()?LISTENING:VOICE_DISABLED;
  xTaskNotifyGive(feedHandle);xTaskNotifyGive(detectHandle);xTaskNotifyGive(idleHandle);
  Serial.printf("[VOICE] Hi ESP ready=%u wake=%s commands=%u psram=%lu\n",unsigned(active()),wakeModel?wakeModel:"none",unsigned(phraseCount),(unsigned long)ESP.getFreePsram());
}
void initTask(void*) {
  // OTA validation happens after 5 s. Voice starts later and never gates BLE recovery.
  while(millis()<8000u || otaBusy() || mediaBusy()) vTaskDelay(pdMS_TO_TICKS(250));
  initialize();
  initHandle=nullptr;
  vTaskDelete(nullptr);
}
void scheduleInitialize() {
  if(initHandle||status.load()==STARTING||status.load()==LISTENING)return;
  if(xTaskCreatePinnedToCore(initTask,"voice-init",8192,nullptr,1,&initHandle,1)!=pdPASS)status=NO_MEMORY;
}

void encode(uint8_t* bytes,size_t length=22){
  memset(bytes,0,length);bytes[0]=0xCD;bytes[1]=2;bytes[2]=status.load();bytes[3]=enabled.load()?1:0;
  portENTER_CRITICAL(&stateMux);put32le(bytes+4,serial);bytes[8]=lastCommand;bytes[9]=lastResult;put32le(bytes+10,lastAt);put16le(bytes+20,lastValue);portEXIT_CRITICAL(&stateMux);
  put32le(bytes+14,discontinuities.load());bytes[18]=ChakshuTransfer::offline.load()?1:0;
}
bool queueLocal(uint8_t operation,uint32_t offset=0) {
  using namespace ChakshuTransfer;
  Request request{};request.local=true;request.localEpoch=localEpoch.load();request.operation=operation;request.offset=offset;
  if(!requests||xQueueSend(requests,&request,0)!=pdTRUE)return false;
  if(offline.load())stopRequested.store(true);
  return true;
}
void tick() {
  if(!otaBusy()) {const int pending=persistEnabled.exchange(-1);if(pending>=0){
    Preferences settings;if(settings.begin("chakshu-voice",false)){settings.putBool("enabled",pending==1);settings.end();}
  }}
  PendingCommand pending;if(!commandQueue||xQueueReceive(commandQueue,&pending,0)!=pdTRUE)return;
  if(!active()||otaBusy()||pending.epoch!=discontinuities.load()||uint32_t(millis()-pending.at)>2000u)return;
  const uint8_t command=pending.command;
  const uint16_t value=command==VIDEO_START?25:0;
  const bool online=deviceConnected.load()&&leaseConnection.load()==connectionGeneration.load()&&
    leaseAt.load()!=0&&uint32_t(millis()-leaseAt.load())<6000u;
  if(command==WAKE){
    portENTER_CRITICAL(&stateMux);++serial;lastCommand=WAKE;lastResult=online?2:0;lastAt=millis();lastValue=0;portEXIT_CRITICAL(&stateMux);
    if(feedbackHandle)xTaskNotifyGive(feedbackHandle);
    if(deviceConnected.load()&&events){uint8_t bytes[22];encode(bytes,sizeof(bytes));events->setValue(bytes,sizeof(bytes));events->notify();}
    Serial.printf("[VOICE] Hi ESP detected online=%u\\n",unsigned(online));
    return;
  }
  uint8_t result=0;
  // Voice always performs local SD-first actions. BLE/PWA presence only adds notification/sync.
  using namespace ChakshuTransfer;
  if(command==STOP||command==VIDEO_STOP||command==AUDIO_OFF){
    ++localEpoch;if(offline.load())stopRequested.store(true);if(streamingEnabled.load())stopStreaming();
  } else if(command==PHOTO||command==DESCRIBE) {
    if(offline.load()||!queueLocal(11))result=1;
    else if(command==DESCRIBE)result=3; // Photo is durable now; inference can occur after sync.
  } else if(command==VIDEO_START) {
    if(!exitRemoteStandby()||!queueLocal(5,uint32_t(25u)<<8))result=1;
  } else if(command==AUDIO_ON) {
    if(!exitRemoteStandby()||!queueLocal(10,600))result=1;
  }
  portENTER_CRITICAL(&stateMux);++serial;lastCommand=command;lastResult=result?result:(online?2:0);lastAt=millis();lastValue=value;portEXIT_CRITICAL(&stateMux);
  if(online&&events){uint8_t bytes[22];encode(bytes,sizeof(bytes));events->setValue(bytes,sizeof(bytes));events->notify();}
}
class Callbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c)override{uint8_t bytes[22];encode(bytes,sizeof(bytes));c->setValue(bytes,sizeof(bytes));}
  void onWrite(BLECharacteristic* c)override{
    const String value=c->getValue();if(value.length()!=3||uint8_t(value[0])!=0xCC||uint8_t(value[1])!=2)return;
    const uint8_t op=value[2];
    if(op==2){leaseConnection=connectionGeneration.load();leaseAt=millis();return;}
    if(op==3){leaseAt=0;return;}
    if(op>1)return;
    enabled=op==1;persistEnabled=op;++discontinuities;leaseAt=0;
    if(mnData)status=enabled?LISTENING:VOICE_DISABLED;
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