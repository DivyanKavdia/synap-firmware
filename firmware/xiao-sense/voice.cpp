// One-model local keyword spotting for Chakshu.
// The classifier is optional, starts only after BLE/OTA boot validation, and maps
// recognized labels to the same SD-first media actions used by the companion app.
namespace ChakshuVoice {
enum Status : uint8_t { STARTING=0,LISTENING=1,MODEL_MISSING=2,NO_MEMORY=3,MODEL_ERROR=4,VOICE_DISABLED=5 };
std::atomic<uint8_t> status{STARTING};
std::atomic<bool> enabled{true},initialized{false};
std::atomic<int> persistEnabled{-1};
std::atomic<uint32_t> discontinuities{0};
struct Block { uint16_t count;int16_t samples[800]; };
struct PendingCommand { uint8_t command;uint16_t value;float confidence;uint32_t at; };
QueueHandle_t pcmQueue=nullptr,commandQueue=nullptr;
TaskHandle_t inferenceHandle=nullptr,idleHandle=nullptr,feedbackHandle=nullptr;
int16_t* ring=nullptr;size_t ringWrite=0,ringCount=0;uint32_t sinceInference=0;
BLECharacteristic* events=nullptr;
portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t serial=0,lastAt=0;uint8_t lastCommand=0,lastResult=0;uint16_t lastValue=0;
constexpr size_t WINDOW_SAMPLES=20480;
constexpr uint32_t START_AFTER_MS=8000;
constexpr uint32_t COMMAND_WINDOW_MS=4500;
constexpr uint32_t DECISION_GAP_MS=450;
constexpr uint32_t ACTION_REFRACTORY_MS=1200;
constexpr uint32_t INFERENCE_STRIDE_SAMPLES=3200;
constexpr float WAKE_THRESHOLD=0.88f;
constexpr float WAKE_STRONG_THRESHOLD=0.96f;
constexpr float COMMAND_THRESHOLD=0.78f;
constexpr float COMMAND_STRONG_THRESHOLD=0.94f;
constexpr float MIN_WAKE_MARGIN=0.25f;
constexpr float MIN_COMMAND_MARGIN=0.18f;
constexpr uint8_t WAKE_LED_PIN=21;
constexpr uint16_t WAKE_LED_MS=90;
constexpr uint8_t MEL_EDGES[10]={3,6,11,16,24,32,43,57,74,96};
constexpr float K_PI=3.14159265358979323846f;

bool active(){return initialized.load()&&status.load()==LISTENING&&enabled.load();}

void resetDecisionState();
void feed(const int16_t* samples,size_t count) {
  if(!active()||!pcmQueue||!samples||!count)return;
  while(count){
    Block block{};block.count=uint16_t(std::min(count,size_t(800)));
    memcpy(block.samples,samples,block.count*sizeof(int16_t));
    if(xQueueSend(pcmQueue,&block,0)!=pdTRUE){++discontinuities;return;}
    samples+=block.count;count-=block.count;
  }
}

void fft256(float* re,float* im) {
  for(unsigned i=1,j=0;i<256;++i){
    unsigned bit=128;for(;j&bit;bit>>=1)j^=bit;j^=bit;
    if(i<j){std::swap(re[i],re[j]);std::swap(im[i],im[j]);}
  }
  for(unsigned len=2;len<=256;len<<=1){
    const float angle=-2.0f*K_PI/float(len);
    const float wlenRe=cosf(angle),wlenIm=sinf(angle);
    for(unsigned i=0;i<256;i+=len){
      float wr=1.0f,wi=0.0f;
      for(unsigned j=0;j<len/2;++j){
        const unsigned a=i+j,b=a+len/2;
        const float vr=re[b]*wr-im[b]*wi,vi=re[b]*wi+im[b]*wr;
        const float ur=re[a],ui=im[a];
        re[a]=ur+vr;im[a]=ui+vi;re[b]=ur-vr;im[b]=ui-vi;
        const float nextWr=wr*wlenRe-wi*wlenIm;
        wi=wr*wlenIm+wi*wlenRe;wr=nextWr;
      }
    }
  }
}

inline int16_t sampleAt(size_t chronological) {
  const size_t oldest=(ringWrite+WINDOW_SAMPLES-ringCount)%WINDOW_SAMPLES;
  return ring[(oldest+chronological)%WINDOW_SAMPLES];
}

bool extractFeatures(float* out) {
  if(ringCount<WINDOW_SAMPLES)return false;
  memset(out,0,sizeof(float)*ChakshuKwsModel::INPUTS);
  float re[256],im[256];
  double absSum=0.0;
  for(size_t i=0;i<WINDOW_SAMPLES;++i)absSum+=abs(int(sampleAt(i)));
  if(absSum/double(WINDOW_SAMPLES)<55.0)return false;
  for(unsigned frame=0;frame<80;++frame){
    const size_t base=size_t(frame)*256;
    int16_t previous=base?sampleAt(base-1):0;
    for(unsigned i=0;i<256;++i){
      const int16_t current=sampleAt(base+i);
      const float pre=float(current)-0.97f*float(previous);
      previous=current;
      const float window=0.54f-0.46f*cosf(2.0f*K_PI*float(i)/255.0f);
      re[i]=(pre/32768.0f)*window;im[i]=0.0f;
    }
    fft256(re,im);
    float power[129];
    for(unsigned k=0;k<=128;++k)power[k]=(re[k]*re[k]+im[k]*im[k])/(256.0f*256.0f);
    const unsigned timeBin=frame/10;
    for(unsigned band=0;band<8;++band){
      const unsigned left=MEL_EDGES[band],center=MEL_EDGES[band+1],right=MEL_EDGES[band+2];
      float energy=0.0f;
      for(unsigned k=left;k<center;++k)energy+=power[k]*float(k-left)/float(center-left);
      for(unsigned k=center;k<right;++k)energy+=power[k]*float(right-k)/float(right-center);
      out[timeBin*8+band]+=log1pf(energy*1000000.0f)*0.1f;
    }
  }
  for(size_t i=0;i<ChakshuKwsModel::INPUTS;++i)
    out[i]=(out[i]-ChakshuKwsModel::FEATURE_MEAN[i])/ChakshuKwsModel::FEATURE_STD[i];
  return true;
}

template<size_t IN,size_t OUT>
void dense(const float* input,const int8_t* weights,float scale,const float* bias,float* output,bool relu){
  for(size_t j=0;j<OUT;++j){
    float sum=bias[j];
    for(size_t i=0;i<IN;++i)sum+=input[i]*float(weights[i*OUT+j])*scale;
    output[j]=relu&&sum<0.0f?0.0f:sum;
  }
}
uint8_t classify(float& confidence,float& margin) {
  float features[ChakshuKwsModel::INPUTS],h1[ChakshuKwsModel::H1],h2[ChakshuKwsModel::H2],logits[ChakshuKwsModel::OUTPUTS];
  if(!extractFeatures(features)){confidence=margin=0.0f;return ChakshuKwsModel::UNKNOWN;}
  dense<ChakshuKwsModel::INPUTS,ChakshuKwsModel::H1>(features,ChakshuKwsModel::W1,ChakshuKwsModel::W1_SCALE,ChakshuKwsModel::B1,h1,true);
  dense<ChakshuKwsModel::H1,ChakshuKwsModel::H2>(h1,ChakshuKwsModel::W2,ChakshuKwsModel::W2_SCALE,ChakshuKwsModel::B2,h2,true);
  dense<ChakshuKwsModel::H2,ChakshuKwsModel::OUTPUTS>(h2,ChakshuKwsModel::W3,ChakshuKwsModel::W3_SCALE,ChakshuKwsModel::B3,logits,false);
  float maximum=logits[0];for(size_t i=1;i<ChakshuKwsModel::OUTPUTS;++i)maximum=std::max(maximum,logits[i]);
  float total=0.0f,probs[ChakshuKwsModel::OUTPUTS];
  for(size_t i=0;i<ChakshuKwsModel::OUTPUTS;++i){probs[i]=expf(logits[i]-maximum);total+=probs[i];}
  uint8_t best=0;float first=-1.0f,second=-1.0f;
  for(uint8_t i=0;i<ChakshuKwsModel::OUTPUTS;++i){
    const float p=probs[i]/total;
    if(p>first){second=first;first=p;best=i;}else if(p>second)second=p;
  }
  confidence=first;margin=first-second;return best;
}

struct DecisionState {
  bool armed=false;uint32_t armedAt=0,lastActionAt=0,lastCandidateAt=0;
  uint8_t candidate=0,hits=0;
} decision;
void resetDecisionState(){decision.armed=false;decision.candidate=0;decision.hits=0;decision.lastCandidateAt=0;}

uint8_t mapLabel(uint8_t label) {
  using namespace ChakshuKwsModel;
  if(label==TAKE_PHOTO)return PHOTO;
  if(label==TAKE_VIDEO)return VIDEO_START;
  if(label==RECORD_AUDIO)return AUDIO_ON;
  if(label==STOP)return ChakshuTransfer::offlineMode.load()==10?AUDIO_OFF:VIDEO_STOP;
  return 0;
}
void queueDetected(uint8_t command,uint16_t value,float confidence,uint32_t now){
  if(!commandQueue)return;
  PendingCommand pending{command,value,confidence,now};
  if(xQueueSend(commandQueue,&pending,0)!=pdTRUE)++discontinuities;
}
void consider(uint8_t label,float confidence,float margin,uint32_t now) {
  if(decision.armed&&uint32_t(now-decision.armedAt)>COMMAND_WINDOW_MS)resetDecisionState();
  if(decision.lastActionAt&&uint32_t(now-decision.lastActionAt)<ACTION_REFRACTORY_MS)return;
  const bool wake=label==ChakshuKwsModel::HEY_SNAP;
  if(!decision.armed&&!wake)return;
  if(decision.armed&&wake)return;
  const float threshold=wake?WAKE_THRESHOLD:COMMAND_THRESHOLD;
  const float strong=wake?WAKE_STRONG_THRESHOLD:COMMAND_STRONG_THRESHOLD;
  const float requiredMargin=wake?MIN_WAKE_MARGIN:MIN_COMMAND_MARGIN;
  if(confidence<threshold||margin<requiredMargin){
    if(uint32_t(now-decision.lastCandidateAt)>DECISION_GAP_MS){decision.candidate=0;decision.hits=0;}
    return;
  }
  if(decision.candidate==label&&uint32_t(now-decision.lastCandidateAt)<=DECISION_GAP_MS)++decision.hits;
  else {decision.candidate=label;decision.hits=1;}
  decision.lastCandidateAt=now;
  if(confidence<strong&&decision.hits<2)return;
  decision.candidate=0;decision.hits=0;
  if(wake){
    decision.armed=true;decision.armedAt=now;queueDetected(WAKE,0,confidence,now);
    return;
  }
  const uint8_t command=mapLabel(label);if(!command)return;
  const uint16_t value=command==VIDEO_START?25:0;
  decision.armed=false;decision.lastActionAt=now;queueDetected(command,value,confidence,now);
}

void inferenceTask(void*) {
  Block block{};
  for(;;){
    if(xQueueReceive(pcmQueue,&block,pdMS_TO_TICKS(250))!=pdTRUE)continue;
    if(!active()||otaBusy()){ringCount=sinceInference=0;continue;}
    for(uint16_t i=0;i<block.count;++i){
      ring[ringWrite]=block.samples[i];ringWrite=(ringWrite+1)%WINDOW_SAMPLES;
      if(ringCount<WINDOW_SAMPLES)++ringCount;
    }
    sinceInference+=block.count;
    if(ringCount<WINDOW_SAMPLES||sinceInference<INFERENCE_STRIDE_SAMPLES)continue;
    sinceInference=0;float confidence=0.0f,margin=0.0f;
    const uint8_t label=classify(confidence,margin);
    consider(label,confidence,margin,millis());
  }
}
bool pulseWakeLed() {
  if(!active()||otaBusy())return false;
  ChakshuResources::Lease admission;if(!admission)return false;
  pinMode(WAKE_LED_PIN,OUTPUT);digitalWrite(WAKE_LED_PIN,LOW);
  vTaskDelay(pdMS_TO_TICKS(WAKE_LED_MS));digitalWrite(WAKE_LED_PIN,HIGH);return true;
}
void feedbackTask(void*) {for(;;){ulTaskNotifyTake(pdTRUE,portMAX_DELAY);pulseWakeLed();}}
void idleTask(void*) {
  int16_t samples[800];
  for(;;){
    if(!active()||streamingEnabled.load()||mediaBusy()||otaBusy()||
       xSemaphoreTakeRecursive(microphoneMutex,0)!=pdTRUE){vTaskDelay(pdMS_TO_TICKS(20));continue;}
    if(active()&&!streamingEnabled.load()&&!mediaBusy()&&!otaBusy()&&startMicrophone()){
      const size_t bytes=microphoneI2S.readBytes(reinterpret_cast<char*>(samples),sizeof(samples));
      if(bytes&&!(bytes&1))feed(samples,bytes/2);else ++discontinuities;
    }
    xSemaphoreGiveRecursive(microphoneMutex);vTaskDelay(1);
  }
}
void cleanup() {
  if(inferenceHandle){vTaskDelete(inferenceHandle);inferenceHandle=nullptr;}
  if(idleHandle){vTaskDelete(idleHandle);idleHandle=nullptr;}
  if(feedbackHandle){vTaskDelete(feedbackHandle);feedbackHandle=nullptr;}
  if(pcmQueue){vQueueDelete(pcmQueue);pcmQueue=nullptr;}
  if(commandQueue){vQueueDelete(commandQueue);commandQueue=nullptr;}
  free(ring);ring=nullptr;ringWrite=ringCount=sinceInference=0;initialized=false;
}
void initialize() {
  if(initialized.load())return;
  Preferences settings;if(settings.begin("chakshu-voice",true)){enabled.store(settings.getBool("enabled",true));settings.end();}
  ring=static_cast<int16_t*>(ps_malloc(WINDOW_SAMPLES*sizeof(int16_t)));
  pcmQueue=xQueueCreate(4,sizeof(Block));commandQueue=xQueueCreate(6,sizeof(PendingCommand));
  if(!ring||!pcmQueue||!commandQueue){status=NO_MEMORY;cleanup();return;}
  memset(ring,0,WINDOW_SAMPLES*sizeof(int16_t));
  if(xTaskCreatePinnedToCore(inferenceTask,"voice-kws",8192,nullptr,1,&inferenceHandle,0)!=pdPASS||
     xTaskCreatePinnedToCore(idleTask,"voice-idle",4096,nullptr,1,&idleHandle,0)!=pdPASS||
     xTaskCreatePinnedToCore(feedbackTask,"voice-led",2048,nullptr,1,&feedbackHandle,0)!=pdPASS){
    status=NO_MEMORY;cleanup();return;
  }
  initialized=true;status=enabled.load()?LISTENING:VOICE_DISABLED;
  Serial.printf("[VOICE] single-model v%u ready=%u model=%s psram=%lu\n",unsigned(ChakshuKwsModel::VERSION),unsigned(active()),ChakshuKwsModel::SHA256,(unsigned long)ESP.getFreePsram());
}
void encode(uint8_t* bytes,size_t length=22){
  memset(bytes,0,length);bytes[0]=0xCD;bytes[1]=2;bytes[2]=status.load();bytes[3]=enabled.load()?1:0;
  portENTER_CRITICAL(&stateMux);put32le(bytes+4,serial);bytes[8]=lastCommand;bytes[9]=lastResult;put32le(bytes+10,lastAt);put16le(bytes+20,lastValue);portEXIT_CRITICAL(&stateMux);
  put32le(bytes+14,discontinuities.load());bytes[18]=ChakshuTransfer::offline.load()?1:0;
}
bool queueLocal(uint8_t operation,uint32_t offset=0) {
  using namespace ChakshuTransfer;
  Request request{};request.local=true;request.localEpoch=localEpoch.load();request.operation=operation;request.offset=offset;
  return requests&&xQueueSend(requests,&request,0)==pdTRUE;
}
void execute(const PendingCommand& pending) {
  uint8_t result=0;uint16_t value=pending.value;
  if(pending.command==WAKE){
    if(feedbackHandle)xTaskNotifyGive(feedbackHandle);
  } else if(pending.command==PHOTO) {
    if(!queueLocal(11))result=1;
  } else if(pending.command==VIDEO_START) {
    if(!exitRemoteStandby()||!queueLocal(5,uint32_t(25)<<8))result=1;value=25;
  } else if(pending.command==AUDIO_ON) {
    if(!exitRemoteStandby()||!queueLocal(10,600))result=1;
  } else if(pending.command==VIDEO_STOP||pending.command==AUDIO_OFF) {
    using namespace ChakshuTransfer;++localEpoch;
    if(offline.load())stopRequested.store(true);
    if(streamingEnabled.load())stopStreaming();
  } else result=1;
  portENTER_CRITICAL(&stateMux);++serial;lastCommand=pending.command;lastResult=result;lastAt=millis();lastValue=value;portEXIT_CRITICAL(&stateMux);
  if(deviceConnected.load()&&events){uint8_t bytes[22];encode(bytes,sizeof(bytes));events->setValue(bytes,sizeof(bytes));events->notify();}
  Serial.printf("[VOICE] command=%u result=%u confidence=%.3f value=%u\n",unsigned(pending.command),unsigned(result),double(pending.confidence),unsigned(value));
}
void tick() {
  if(!initialized.load()&&status.load()==STARTING&&ChakshuResources::runtimeReady.load()&&millis()>=START_AFTER_MS&&!otaBusy())initialize();
  if(!otaBusy()){const int pending=persistEnabled.exchange(-1);if(pending>=0){
    Preferences settings;if(settings.begin("chakshu-voice",false)){settings.putBool("enabled",pending==1);settings.end();}
  }}
  PendingCommand command;if(commandQueue&&xQueueReceive(commandQueue,&command,0)==pdTRUE&&active()&&!otaBusy())execute(command);
}
class Callbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c)override{uint8_t bytes[22];encode(bytes,sizeof(bytes));c->setValue(bytes,sizeof(bytes));}
  void onWrite(BLECharacteristic* c)override{
    const String value=c->getValue();if(value.length()!=3||uint8_t(value[0])!=0xCC||uint8_t(value[1])!=2)return;
    const uint8_t op=value[2];if(op==2||op==3)return;if(op>1)return;
    enabled=op==1;persistEnabled=op;++discontinuities;resetDecisionState();
    if(initialized.load())status=enabled?LISTENING:VOICE_DISABLED;
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
