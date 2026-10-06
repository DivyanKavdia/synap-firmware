// Odyssey C3 clean-room offline SD recorder.
// Write-only by design: no catalogue, BLE file reads, delete/clear, journals,
// preallocation, segmentation, or custom SD protocol recovery.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>

static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState(){return odysseySdBootState.load();}
uint8_t odysseySdProbeState(){return odysseySdProbeStage.load();}

#if CONFIG_IDF_TARGET_ESP32C3
constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN,ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN,ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static constexpr uint32_t ODYSSEY_SD_STARTUP_SETTLE_MS=3000u;
static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=1000000u;
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=512u;
static constexpr size_t ODYSSEY_WAV_HEADER_BYTES=44u;
static constexpr uint32_t ODYSSEY_SD_FLUSH_INTERVAL_MS=2000u;
static SPIClass odysseySdSpi(FSPI);
static std::atomic<bool> odysseySdMounted{false};
static uint8_t odysseySdPcmBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];

static void freshSdState(uint8_t state,uint8_t probe){
  odysseySdBootState=state;odysseySdProbeStage=probe;
}
static void freshSdRelease(){
  SD.end();odysseySdSpi.end();
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  odysseySdMounted=false;
}
static bool freshSdMount(const char* reason){
  if(odysseySdMounted.load()&&odysseySdBootState.load()==1)return true;
  freshSdState(0,0);freshSdRelease();
  const uint32_t now=millis();
  if(now<ODYSSEY_SD_STARTUP_SETTLE_MS)delay(ODYSSEY_SD_STARTUP_SETTLE_MS-now);
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  if(!odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS)){
    freshSdState(2,1);Serial.printf("[SD] fresh %s SPI begin failed\n",reason);return false;
  }
  if(!SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,"/odyssey-sd",2,false)){
    freshSdState(2,2);Serial.printf("[SD] fresh %s mount failed\n",reason);freshSdRelease();return false;
  }
  if(SD.cardType()==CARD_NONE){
    freshSdState(3,0);Serial.printf("[SD] fresh %s no card\n",reason);freshSdRelease();return false;
  }
  if(!SD.exists("/synap")&&!SD.mkdir("/synap")){
    freshSdState(2,4);Serial.printf("[SD] fresh %s /synap create failed\n",reason);freshSdRelease();return false;
  }
  odysseySdMounted=true;freshSdState(1,6);markOdysseySdBatteryDividerPresent();
  Serial.printf("[SD] fresh %s ready at %lu Hz card=%lluMB\n",reason,
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ),
    static_cast<unsigned long long>(SD.cardSize()/(1024ull*1024ull)));
  return true;
}
static void freshWavHeader(uint8_t* h,uint32_t bytes){
  memset(h,0,44);memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);h[20]=1;h[22]=1;
  put32le(h+24,SAMPLE_RATE);put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static bool freshWriteAll(File& file,const uint8_t* data,size_t size){
  while(size){const size_t n=file.write(data,size);if(!n)return false;data+=n;size-=n;}return true;
}
static bool freshDrain(File& file,size_t& buffered,uint32_t& bytes,bool finalDrain){
  while(buffered){
    const size_t offset=ODYSSEY_WAV_HEADER_BYTES+size_t(bytes);
    const size_t sectorOffset=offset&(ODYSSEY_SD_WRITE_CHUNK_BYTES-1u);
    size_t chunk=0;
    if(sectorOffset){
      const size_t boundary=ODYSSEY_SD_WRITE_CHUNK_BYTES-sectorOffset;
      if(!finalDrain&&buffered<boundary)return true;
      chunk=std::min(buffered,boundary);
    }else if(buffered>=ODYSSEY_SD_WRITE_CHUNK_BYTES)chunk=ODYSSEY_SD_WRITE_CHUNK_BYTES;
    else if(finalDrain)chunk=buffered;
    else return true;
    if(!freshWriteAll(file,odysseySdPcmBuffer,chunk))return false;
    bytes+=uint32_t(chunk);buffered-=chunk;
    if(buffered)memmove(odysseySdPcmBuffer,odysseySdPcmBuffer+chunk,buffered);
  }
  return true;
}
static bool freshPaths(char* temp,size_t tempSize,char* wav,size_t wavSize){
  for(uint8_t attempt=0;attempt<12;++attempt){
    const uint32_t a=esp_random(),b=esp_random();
    snprintf(temp,tempSize,"/synap/rec_%08lx_%08lx.tmp",(unsigned long)a,(unsigned long)b);
    snprintf(wav,wavSize,"/synap/rec_%08lx_%08lx.wav",(unsigned long)a,(unsigned long)b);
    if(!SD.exists(temp)&&!SD.exists(wav))return true;
  }
  return false;
}
static bool freshRecordTake(uint8_t& stage,uint32_t& bytes){
  stage=0;bytes=0;
  if(!freshSdMount("record")){stage=1;return false;}
  char temp[72]{},wav[72]{};
  if(!freshPaths(temp,sizeof(temp),wav,sizeof(wav))){stage=2;return false;}
  File file=SD.open(temp,FILE_WRITE);
  if(!file){stage=2;return false;}
  (void)file.setBufferSize(ODYSSEY_SD_WRITE_CHUNK_BYTES);
  uint8_t header[44];freshWavHeader(header,0);
  if(!freshWriteAll(file,header,sizeof(header))){stage=3;file.close();return false;}
  file.flush();

  bool failed=false;size_t buffered=0;uint32_t lastFlush=millis();
  applyCpuPowerProfile(true);
#if USE_REAL_I2S_MIC
  {
    MicrophoneGuard microphone;
    if(!startMicrophone()){stage=4;failed=true;}
    else{
      odysseyRecordingStartedAt=millis();odysseyCaptureActive=true;odysseySdRecoveryActive=false;
      updateStatusLed(true);Serial.printf("[SD] fresh PCM capture active file=%s\n",wav);
      int32_t raw[SAMPLES_PER_FRAME];int16_t pcm[SAMPLES_PER_FRAME];
      while(!failed&&!odysseyStopRequested.load()){
        size_t received=0;uint8_t empty=0;
        while(received<sizeof(raw)&&!odysseyStopRequested.load()){
          const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
          if(!n){if(++empty>=4){stage=4;failed=true;break;}}
          else{received+=n;empty=0;}
        }
        if(failed||odysseyStopRequested.load())break;
        for(uint16_t i=0;i<SAMPLES_PER_FRAME;++i)pcm[i]=static_cast<int16_t>(raw[i]>>16);
        if(buffered+sizeof(pcm)>sizeof(odysseySdPcmBuffer)&&!freshDrain(file,buffered,bytes,false)){
          stage=5;failed=true;break;
        }
        if(buffered+sizeof(pcm)>sizeof(odysseySdPcmBuffer)){stage=5;failed=true;break;}
        memcpy(odysseySdPcmBuffer+buffered,pcm,sizeof(pcm));buffered+=sizeof(pcm);
        if(!freshDrain(file,buffered,bytes,false)){stage=5;failed=true;break;}
        if(uint32_t(millis()-lastFlush)>=ODYSSEY_SD_FLUSH_INTERVAL_MS){file.flush();lastFlush=millis();}
      }
      odysseyCaptureActive=false;updateStatusLed(true);stopMicrophone();
    }
  }
#else
  stage=4;failed=true;
#endif
  if(!failed&&(!freshDrain(file,buffered,bytes,true)||!bytes)){stage=bytes?5:7;failed=true;}
  if(!failed){
    file.flush();
    if(!file.seek(0)){stage=6;failed=true;}
    else{
      freshWavHeader(header,bytes);
      if(!freshWriteAll(file,header,sizeof(header))){stage=6;failed=true;}
      else{file.flush();if(file.size()!=ODYSSEY_WAV_HEADER_BYTES+size_t(bytes)){stage=6;failed=true;}}
    }
  }
  file.close();
  if(!failed&&!SD.rename(temp,wav)){stage=6;failed=true;}
  if(failed&&!bytes)(void)SD.remove(temp);
  applyCpuPowerProfile(false);
  if(failed){
    Serial.printf("[SD] fresh local audio failed stage=%u bytes=%lu\n",unsigned(stage),(unsigned long)bytes);
    freshSdState(2,4);freshSdRelease();return false;
  }
  Serial.printf("[SD] fresh local audio saved file=%s pcm=%lu bytes\n",wav,(unsigned long)bytes);
  return true;
}
static void freshRecordTask(void*){
  uint8_t stage=0;uint32_t bytes=0;
  const bool saved=!odysseyStopRequested.load()&&freshRecordTake(stage,bytes);
  const uint32_t done=millis();
  odysseyCaptureActive=false;odysseySdRecoveryActive=false;
  odysseySdSleepGuardUntil=done+2000u;disconnectedAt=done;
  odysseyRecording=false;odysseyStopRequested=false;applyCpuPowerProfile(false);
  if(!saved&&stage!=7)odysseyRecordFaultAt=done;
  updateStatusLed(true);vTaskDelete(nullptr);
}
void odysseyInitializeSdCardBeforeBle(){
  odysseySdRecoveryActive=true;updateStatusLed(true);
  const bool ready=freshSdMount("boot");
  odysseySdRecoveryActive=false;if(!ready)odysseyRecordFaultAt=millis();updateStatusLed(true);
}
void odysseyToggleRecording(){
  if(odysseyRecording.load()){odysseyStopRequested=true;Serial.println("[TOUCH] double tap -> fresh SD audio STOP");return;}
  if(deviceConnected.load()||streamingEnabled.load()||otaBusy()||sleepPending||batteryCritical())return;
  odysseyRecordFaultAt=0;odysseyStopRequested=false;odysseyCaptureActive=false;
  odysseySdRecoveryActive=true;odysseyRecording=true;updateStatusLed(true);
  Serial.println(odysseySdMounted.load()?"[TOUCH] double tap -> fresh SD audio START":"[TOUCH] double tap -> fresh SD mount + audio START");
  if(xTaskCreate(freshRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS){
    odysseyRecording=false;odysseySdRecoveryActive=false;odysseyRecordFaultAt=millis();
    updateStatusLed(true);Serial.println("[SD] fresh recorder task creation failed");
  }
}
bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs){
  if(!odysseyRecording.load())return true;
  odysseyStopRequested=true;Serial.println("[SD] finalizing fresh offline WAV before live audio");
  const uint32_t started=millis();
  while(odysseyRecording.load()&&uint32_t(millis()-started)<timeoutMs)delay(10);
  return !odysseyRecording.load();
}
bool odysseyPrepareSdForPowerTransition(uint32_t){
  if(odysseyRecording.load())return false;
  freshSdRelease();freshSdState(0,0);return true;
}
namespace OdysseyTransfer{
void initialize(){}
void ble(BLEService*){}
bool available(){return false;}
}

#elif CONFIG_IDF_TARGET_ESP32S3
constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN,ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN,ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static SPIClass odysseySdSpi(FSPI);
void odysseyDetectSdCard(){
  odysseySdBootState=0;odysseySdProbeStage=0;SD.end();odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);pinMode(ODYSSEY_SD_CS,OUTPUT);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,400000,"/odyssey-sd",1,false);
  if(mounted&&SD.cardType()!=CARD_NONE){odysseySdBootState=1;odysseySdProbeStage=6;Serial.println("[SD] Odyssey S3 detection succeeded");}
  else if(mounted){odysseySdBootState=3;Serial.println("[SD] Odyssey S3 no card reported");}
  else{odysseySdBootState=2;Serial.println("[SD] Odyssey S3 detection/mount failed");}
  SD.end();odysseySdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
}
#else
#error Unsupported Odyssey SD target
#endif
#endif
