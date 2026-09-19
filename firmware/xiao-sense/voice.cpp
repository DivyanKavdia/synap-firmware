// Lightweight Chakshu TinyML voice runtime.
// No ESP-SR, AFE, TFLite Micro or external model payload: ~2.2 KB int8 learned weights.
#include <math.h>
namespace ChakshuVoice {
using namespace ChakshuTinyModel;
enum Status : uint8_t { STARTING=0,LISTENING=1,MODEL_MISSING=2,NO_MEMORY=3,MODEL_ERROR=4,VOICE_DISABLED=5 };
std::atomic<uint8_t> status{MODEL_MISSING};
std::atomic<bool> enabled{true};
std::atomic<int> persistEnabled{-1};
std::atomic<uint32_t> discontinuities{0},leaseAt{0},leaseConnection{0};
std::atomic<uint16_t> audioMeanAbs{0},audioPeak{0},candidateConfidence{0};
std::atomic<uint8_t> candidateId{0};
std::atomic<uint32_t> candidateAt{0},candidateCount{0};
std::atomic<uint32_t> mediaCompletion{0};
struct Block { uint16_t count;int16_t samples[800]; };
struct PendingCommand { uint8_t command;uint32_t epoch,at; };
struct Inference { uint8_t cls;float confidence,margin; };
struct AcLevel { uint16_t meanAbs,peak; };
QueueHandle_t pcmQueue=nullptr,commandQueue=nullptr;
TaskHandle_t workerHandle=nullptr,idleHandle=nullptr,initHandle=nullptr;
int16_t* ring=nullptr;
size_t writeAt=0,samplesSeen=0;
uint32_t samplesSinceInference=0,speechHoldUntil=0;
float noiseFloor=180.0f;
uint8_t vadRun=0;
uint8_t streakClass=NOISE,streakCount=0;
uint32_t streakAt=0;
BLECharacteristic* events=nullptr;
BLECharacteristic* diagnostics=nullptr;
portMUX_TYPE stateMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t serial=0,lastAt=0;uint8_t lastCommand=0,lastResult=0;uint16_t lastValue=0;
Gate gate;

bool active(){return status.load()==LISTENING&&enabled.load()&&ring&&workerHandle;}

void resetWindow(){
  writeAt=0;samplesSeen=0;samplesSinceInference=0;speechHoldUntil=0;vadRun=0;
  streakClass=NOISE;streakCount=0;streakAt=0;gate.reset();
}
inline int16_t sampleAt(size_t relative){
  return ring[(writeAt+relative)%WINDOW_SAMPLES];
}
AcLevel measureAcLevel(const int16_t* samples,uint16_t count){
  if(!samples||!count)return {0,0};
  int64_t sum=0;
  for(uint16_t i=0;i<count;++i)sum+=samples[i];
  const int32_t mean=int32_t(sum/int64_t(count));
  uint64_t absSum=0;uint32_t peak=0;
  for(uint16_t i=0;i<count;++i){
    const int32_t centered=int32_t(samples[i])-mean;
    const uint32_t magnitude=uint32_t(centered<0?-int64_t(centered):centered);
    absSum+=magnitude;if(magnitude>peak)peak=magnitude;
  }
  const uint64_t meanAbs=absSum/count;
  return {uint16_t(meanAbs>65535u?65535u:meanAbs),uint16_t(peak>65535u?65535u:peak)};
}
float windowGain(){
  double sum=0.0;
  for(size_t i=0;i<WINDOW_SAMPLES;++i)sum+=double(sampleAt(i));
  const double mean=sum/double(WINDOW_SAMPLES);
  double power=0.0;
  for(size_t i=0;i<WINDOW_SAMPLES;++i){
    const double centered=(double(sampleAt(i))-mean)/32768.0;
    power+=centered*centered;
  }
  const float rms=sqrtf(float(power/double(WINDOW_SAMPLES))+1e-12f);
  if(rms<0.004f)return 1.0f;
  return fminf(10.0f,0.10f/rms);
}
float updateNoiseFloor(float floor,uint16_t meanAbs,bool held){
  if(held)return floor;
  const float level=float(meanAbs);
  if(level<floor)return floor*0.90f+level*0.10f;
  if(level<floor*1.60f)return floor*0.995f+level*0.005f;
  return floor;
}
float voiceThreshold(float floor){
  return fmaxf(220.0f,floor*2.0f+80.0f);
}
Inference infer(){
  float input[TIME_FRAMES][BANDS];
  float c1[TIME_FRAMES][CHANNELS];
  float c2[TIME_FRAMES][CHANNELS];
  const float gain=windowGain();
  for(uint8_t t=0;t<TIME_FRAMES;++t){
    const size_t start=size_t(t)*FRAME_HOP;
    float mean=0.0f;
    for(uint16_t n=0;n<FRAME_SAMPLES;++n)mean+=float(sampleAt(start+n))/32768.0f;
    mean/=float(FRAME_SAMPLES);
    for(uint8_t band=0;band<BANDS;++band){
      float q1=0.0f,q2=0.0f;
      const float coeff=GOERTZEL_COEFF[band];
      for(uint16_t n=0;n<FRAME_SAMPLES;++n){
        const float x=(float(sampleAt(start+n))/32768.0f-mean)*gain;
        const float q0=coeff*q1-q2+x;q2=q1;q1=q0;
      }
      const float power=fmaxf(0.0f,q1*q1+q2*q2-coeff*q1*q2)/
        float(uint32_t(FRAME_SAMPLES)*uint32_t(FRAME_SAMPLES));
      const float feature=log1pf(power*100000.0f);
      input[t][band]=(feature-FEATURE_MEAN[band])*FEATURE_INV_STD[band];
    }
  }
  for(uint8_t t=0;t<TIME_FRAMES;++t)for(uint8_t out=0;out<CHANNELS;++out){
    float sum=C1_BIAS[out];
    for(uint8_t in=0;in<BANDS;++in)for(uint8_t k=0;k<3;++k){
      const int ti=int(t)+int(k)-1;if(ti<0||ti>=TIME_FRAMES)continue;
      const size_t wi=(size_t(out)*BANDS+in)*3u+k;
      sum+=C1_SCALE*float(C1_WEIGHT[wi])*input[ti][in];
    }
    c1[t][out]=fmaxf(0.0f,sum);
  }
  for(uint8_t t=0;t<TIME_FRAMES;++t)for(uint8_t out=0;out<CHANNELS;++out){
    float sum=C2_BIAS[out];
    for(uint8_t in=0;in<CHANNELS;++in)for(uint8_t k=0;k<3;++k){
      const int ti=int(t)+int(k)-1;if(ti<0||ti>=TIME_FRAMES)continue;
      const size_t wi=(size_t(out)*CHANNELS+in)*3u+k;
      sum+=C2_SCALE*float(C2_WEIGHT[wi])*c1[ti][in];
    }
    c2[t][out]=fmaxf(0.0f,sum);
  }
  float pooled[CHANNELS*2];
  for(uint8_t ch=0;ch<CHANNELS;++ch){
    float maximum=c2[0][ch],mean=0.0f;
    for(uint8_t t=0;t<TIME_FRAMES;++t){maximum=fmaxf(maximum,c2[t][ch]);mean+=c2[t][ch];}
    pooled[ch]=maximum;pooled[CHANNELS+ch]=mean/float(TIME_FRAMES);
  }
  float logits[CLASSES],top=-1e30f,second=-1e30f;
  uint8_t best=0;
  for(uint8_t out=0;out<CLASSES;++out){
    float sum=FC_BIAS[out];
    for(uint8_t in=0;in<CHANNELS*2;++in)sum+=FC_SCALE*float(FC_WEIGHT[size_t(out)*CHANNELS*2u+in])*pooled[in];
    logits[out]=sum;
    if(sum>top){second=top;top=sum;best=out;}else if(sum>second)second=sum;
  }
  float denom=0.0f,bestExp=0.0f,secondExp=0.0f;
  for(uint8_t i=0;i<CLASSES;++i){const float e=expf(logits[i]-top);denom+=e;if(i==best)bestExp=e;}
  for(uint8_t i=0;i<CLASSES;++i)if(i!=best)secondExp=fmaxf(secondExp,expf(logits[i]-top));
  return {best,bestExp/fmaxf(denom,1e-6f),(bestExp-secondExp)/fmaxf(denom,1e-6f)};
}
uint8_t classCommand(uint8_t cls){
  if(cls==ChakshuTinyModel::HEY_SNAP)return WAKE;
  if(cls==ChakshuTinyModel::PHOTO)return PHOTO;
  if(cls==ChakshuTinyModel::VIDEO)return VIDEO_START;
  if(cls==ChakshuTinyModel::STOP)return STOP;
  return 0;
}
void consider(const Inference& result,uint32_t now,uint32_t epoch){
  const uint8_t command=classCommand(result.cls);
  candidateId=command;
  candidateConfidence=uint16_t(fminf(1.0f,fmaxf(0.0f,result.confidence))*1000.0f);
  candidateAt=now;++candidateCount;
  const float threshold=command==WAKE?0.72f:0.76f;
  if(!command||result.confidence<threshold||result.margin<0.10f){
    streakClass=NOISE;streakCount=0;return;
  }
  if(streakClass==result.cls&&uint32_t(now-streakAt)<=800u)++streakCount;
  else{streakClass=result.cls;streakCount=1;}
  streakAt=now;
  if(streakCount<2)return;
  streakCount=0;
  const uint8_t accepted=gate.accept(command,result.confidence,now);
  if(accepted){PendingCommand pending{accepted,epoch,now};xQueueSend(commandQueue,&pending,0);}
}
void feed(const int16_t* samples,size_t count){
  if(!active()||!pcmQueue||!samples)return;
  while(count){
    Block block;block.count=uint16_t(std::min(count,size_t(800)));
    memcpy(block.samples,samples,block.count*2);
    if(xQueueSend(pcmQueue,&block,0)!=pdTRUE){++discontinuities;return;}
    samples+=block.count;count-=block.count;
  }
}
void workerTask(void*){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  Block block;uint32_t epoch=discontinuities.load();
  for(;;){
    if(xQueueReceive(pcmQueue,&block,pdMS_TO_TICKS(200))!=pdTRUE)continue;
    if(!active()||otaBusy())continue;
    if(epoch!=discontinuities.load()){epoch=discontinuities.load();resetWindow();xQueueReset(pcmQueue);}
    for(uint16_t i=0;i<block.count;++i){
      ring[writeAt]=block.samples[i];writeAt=(writeAt+1)%WINDOW_SAMPLES;
      if(samplesSeen<WINDOW_SAMPLES)++samplesSeen;
    }
    // INMP441/board paths can carry a sizeable DC offset. The model removes
    // per-frame DC before spectral inference, so VAD and diagnostics must use
    // the same AC-only signal or silence can look like permanent speech.
    const AcLevel level=measureAcLevel(block.samples,block.count);
    const uint16_t meanAbs=level.meanAbs;
    audioMeanAbs=meanAbs;audioPeak=level.peak;
    const uint32_t now=millis();
    const bool held=int32_t(speechHoldUntil-now)>0;
    noiseFloor=updateNoiseFloor(noiseFloor,meanAbs,held);
    const float speechThreshold=voiceThreshold(noiseFloor);
    if(float(meanAbs)>speechThreshold) {
      if(vadRun<255)++vadRun;
      if(vadRun>=2)speechHoldUntil=now+1000u;
    } else {
      vadRun=0;
      if(!held){
        candidateId=0;candidateConfidence=0;
        streakClass=NOISE;streakCount=0;
      }
    }
    samplesSinceInference+=block.count;
    if(samplesSeen>=WINDOW_SAMPLES&&samplesSinceInference>=3200u&&int32_t(speechHoldUntil-now)>0){
      samplesSinceInference=0;consider(infer(),now,epoch);
    }
  }
}
void idleTask(void*){
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
  int16_t samples[800];
  for(;;){
    if(!active()||streamingEnabled.load()||mediaBusy()||otaBusy()||
       xSemaphoreTakeRecursive(microphoneMutex,0)!=pdTRUE){vTaskDelay(pdMS_TO_TICKS(20));continue;}
    if(active()&&!streamingEnabled.load()&&!mediaBusy()&&!otaBusy()&&startMicrophone()){
      const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(samples),sizeof(samples));
      if(n&&!(n&1))feed(samples,n/2);else ++discontinuities;
    }
    xSemaphoreGiveRecursive(microphoneMutex);vTaskDelay(1);
  }
}
void cleanup(){
  if(workerHandle){vTaskDelete(workerHandle);workerHandle=nullptr;}
  if(idleHandle){vTaskDelete(idleHandle);idleHandle=nullptr;}
  if(pcmQueue){vQueueDelete(pcmQueue);pcmQueue=nullptr;}
  if(commandQueue){vQueueDelete(commandQueue);commandQueue=nullptr;}
  free(ring);ring=nullptr;resetWindow();
}
void initialize(){
  status=STARTING;
  Preferences settings;if(settings.begin("chakshu-voice",true)){enabled.store(settings.getBool("enabled",true));settings.end();}
  if(MODEL_SAMPLE_RATE!=16000||LEARNED_WEIGHT_BYTES>3072u){status=MODEL_ERROR;return;}
  if(ESP.getFreePsram()<256u*1024u||ESP.getFreeHeap()<32000u){status=NO_MEMORY;return;}
  ring=static_cast<int16_t*>(ps_malloc(size_t(WINDOW_SAMPLES)*sizeof(int16_t)));
  pcmQueue=xQueueCreate(6,sizeof(Block));commandQueue=xQueueCreate(4,sizeof(PendingCommand));
  if(!ring||!pcmQueue||!commandQueue||
     xTaskCreatePinnedToCore(workerTask,"tiny-voice",12288,nullptr,1,&workerHandle,1)!=pdPASS||
     xTaskCreatePinnedToCore(idleTask,"tiny-listen",4096,nullptr,1,&idleHandle,1)!=pdPASS){
    status=NO_MEMORY;cleanup();return;
  }
  memset(ring,0,size_t(WINDOW_SAMPLES)*sizeof(int16_t));resetWindow();
  status=enabled.load()?LISTENING:VOICE_DISABLED;
  xTaskNotifyGive(workerHandle);xTaskNotifyGive(idleHandle);
  Serial.printf("[VOICE] TinyML ready=%u weights=%lu ring=%lu heap=%lu psram=%lu\n",
    unsigned(active()),(unsigned long)LEARNED_WEIGHT_BYTES,
    (unsigned long)(WINDOW_SAMPLES*sizeof(int16_t)),(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getFreePsram());
}
void initTask(void*){
  while(millis()<12000u||otaBusy()||mediaBusy())vTaskDelay(pdMS_TO_TICKS(250));
  initialize();initHandle=nullptr;vTaskDelete(nullptr);
}
void scheduleInitialize(){
  if(initHandle||status.load()==STARTING||status.load()==LISTENING)return;
  if(xTaskCreatePinnedToCore(initTask,"tiny-init",3072,nullptr,1,&initHandle,1)!=pdPASS)status=NO_MEMORY;
}
void encode(uint8_t* bytes,size_t length=22){
  memset(bytes,0,length);bytes[0]=0xCD;bytes[1]=2;bytes[2]=status.load();bytes[3]=enabled.load()?1:0;
  portENTER_CRITICAL(&stateMux);put32le(bytes+4,serial);bytes[8]=lastCommand;bytes[9]=lastResult;put32le(bytes+10,lastAt);put16le(bytes+20,lastValue);portEXIT_CRITICAL(&stateMux);
  put32le(bytes+14,discontinuities.load());bytes[18]=ChakshuTransfer::offline.load()?1:0;
}
void encodeDiagnostics(uint8_t* bytes,size_t length=20){
  memset(bytes,0,length);bytes[0]=0xCE;bytes[1]=1;bytes[2]=status.load();bytes[3]=enabled.load()?1:0;
  put16le(bytes+4,audioMeanAbs.load());put16le(bytes+6,audioPeak.load());bytes[8]=candidateId.load();
  put16le(bytes+9,candidateConfidence.load());put32le(bytes+11,candidateAt.load());put32le(bytes+15,candidateCount.load());
  bytes[19]=active()?1:0;
}
class DiagnosticCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c)override{uint8_t bytes[20];encodeDiagnostics(bytes,sizeof(bytes));c->setValue(bytes,sizeof(bytes));}
};
bool queueLocal(uint8_t operation,uint32_t offset=0){
  using namespace ChakshuTransfer;
  Request request{};request.local=true;request.localEpoch=localEpoch.load();request.operation=operation;request.offset=offset;
  if(!requests||xQueueSend(requests,&request,0)!=pdTRUE)return false;
  if(offline.load())stopRequested.store(true);
  return true;
}
void mediaCompleted(uint8_t operation,uint8_t error){
  const uint8_t command=operation==11?PHOTO:operation==5?VIDEO_START:0;
  if(command)mediaCompletion.store(0x10000u|(uint32_t(command)<<8)|error);
}
void tick(){
  if(!otaBusy()){const int pending=persistEnabled.exchange(-1);if(pending>=0){
    Preferences settings;if(settings.begin("chakshu-voice",false)){settings.putBool("enabled",pending==1);settings.end();}
  }}
  // Publish completion only after the SD worker has closed the capture files.
  const uint32_t completed=mediaCompletion.exchange(0);
  if(completed){
    const uint8_t error=uint8_t(completed);
    portENTER_CRITICAL(&stateMux);++serial;lastCommand=uint8_t(completed>>8);lastResult=error?1:3;lastAt=millis();lastValue=error;portEXIT_CRITICAL(&stateMux);
    if(deviceConnected.load()&&events){uint8_t bytes[22];encode(bytes,sizeof(bytes));events->setValue(bytes,sizeof(bytes));events->notify();}
  }
  PendingCommand pending;if(!commandQueue||xQueueReceive(commandQueue,&pending,0)!=pdTRUE)return;
  if(!active()||otaBusy()||pending.epoch!=discontinuities.load()||uint32_t(millis()-pending.at)>2200u)return;
  const uint8_t command=pending.command;
  const uint16_t value=command==VIDEO_START?10:0;
  const bool online=deviceConnected.load()&&leaseConnection.load()==connectionGeneration.load()&&
    leaseAt.load()!=0&&uint32_t(millis()-leaseAt.load())<6000u;
  if(command==WAKE){
    portENTER_CRITICAL(&stateMux);++serial;lastCommand=WAKE;lastResult=online?2:0;lastAt=millis();lastValue=0;portEXIT_CRITICAL(&stateMux);
    if(deviceConnected.load()&&events){uint8_t bytes[22];encode(bytes,sizeof(bytes));events->setValue(bytes,sizeof(bytes));events->notify();}
    Serial.printf("[VOICE] TinyML Hey Snap confidence=%u online=%u\n",unsigned(candidateConfidence.load()),unsigned(online));return;
  }
  uint8_t result=0;using namespace ChakshuTransfer;
  if(command==STOP){++localEpoch;if(offline.load())stopRequested.store(true);if(streamingEnabled.load())stopStreaming();}
  else if(command==PHOTO){
    if(!ChakshuStorage::ready)result=ChakshuMedia::NO_SD;
    else if(streamingEnabled.load()||offline.load()||!queueLocal(11))result=ChakshuMedia::BUSY;
  }
  else if(command==VIDEO_START){
    if(!ChakshuStorage::ready)result=ChakshuMedia::NO_SD;
    else if(streamingEnabled.load()||!exitRemoteStandby()||!queueLocal(5,uint32_t(10u)<<8))result=ChakshuMedia::BUSY;
  }
  portENTER_CRITICAL(&stateMux);++serial;lastCommand=command;lastResult=result?1:(online?2:0);lastAt=millis();lastValue=result?result:value;portEXIT_CRITICAL(&stateMux);
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
    if(workerHandle)status=enabled?LISTENING:VOICE_DISABLED;
  }
};
void ble(BLEService* service){
  auto* control=service->createCharacteristic("4fa12356-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_WRITE);
  control->setCallbacks(new Callbacks());
  events=service->createCharacteristic("4fa12357-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_NOTIFY);
  diagnostics=service->createCharacteristic("4fa12358-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  diagnostics->setCallbacks(new DiagnosticCallbacks());
#if defined(CONFIG_BLUEDROID_ENABLED)
  events->addDescriptor(new BLE2902());
#endif
}
}
