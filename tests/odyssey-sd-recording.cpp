#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
constexpr uint32_t SAMPLE_RATE=16000;
constexpr uint16_t SAMPLES_PER_FRAME=800;
constexpr int FILE_WRITE=1,pdPASS=1;
std::atomic<bool> odysseyRecording{false},odysseyStopRequested{false},deviceConnected{false},streamingEnabled{false};
bool sleepPending=false,critical=false,ota=false,micOk=true,cardOk=true,openOk=true,shortWrite=false,allocOk=true,reconnect=false;
uint8_t odysseySdBootState=1;
uint32_t clockMs=0;
int reads=0,micStarts=0,micStops=0,closed=0,sdEnds=0,collisions=0,powerActive=0,powerIdle=0,detectCalls=0;
size_t pos=0;
std::vector<uint8_t> data;
std::string opened;
void (*pendingTask)(void*)=nullptr;
struct Logger {void println(const char*){} template<class... T> void printf(const char*,T...){} } Serial;
void put32le(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=v>>(i*8);}
uint32_t get32(size_t p){return uint32_t(data[p])|(uint32_t(data[p+1])<<8)|(uint32_t(data[p+2])<<16)|(uint32_t(data[p+3])<<24);}
struct File {
 bool valid=false;
 explicit operator bool()const{return valid;}
 size_t write(const uint8_t* b,size_t n){
  if(shortWrite && n>44)n=8;
  if(pos+n>data.size())data.resize(pos+n);
  memcpy(data.data()+pos,b,n);pos+=n;return n;
 }
 bool seek(size_t p){pos=p;return true;}
 void flush(){}
 void close(){++closed;valid=false;}
};
struct Storage {
 uint8_t cardType(){return cardOk?3:0;}
 bool exists(const char* p){if(std::string(p)=="/synap")return true;if(collisions){--collisions;return true;}return false;}
 bool mkdir(const char*){return true;}
 File open(const char* p,int){opened=p;pos=0;data.clear();return File{openOk};}
 void end(){++sdEnds;}
} SD;
void odysseyDetectSdCard(){++detectCalls;odysseySdBootState=cardOk?1:2;}
uint8_t odysseySdDetectionState(){return odysseySdBootState;}
uint32_t esp_random(){static uint32_t n=0;return ++n;}
uint32_t millis(){return clockMs;}
bool otaBusy(){return ota;}
bool batteryCritical(){return critical;}
void applyCpuPowerProfile(bool active){if(active)++powerActive;else ++powerIdle;}
struct MicrophoneGuard { ~MicrophoneGuard(){} };
bool startMicrophone(){++micStarts;return micOk;}
void stopMicrophone(){++micStops;}
struct Mic {
 size_t readBytes(char* out,size_t n){
  ++reads;clockMs+=1000;
  if(reconnect)deviceConnected=true;
  if(reads>4){odysseyStopRequested=true;return 0;}
  // Partial I2S reads, with alternating positive/negative extremes.
  n=std::min(n,size_t(1600));
  for(size_t i=0;i<n;i+=4){uint32_t v=(i%8)?0x80000000u:0x7fff0000u;memcpy(out+i,&v,4);}
  return n;
 }
} microphoneI2S;
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){if(!allocOk)return 0;pendingTask=fn;return pdPASS;}
void vTaskDelete(void*){}
// INSERT RECORDER
void reset(){
 odysseyRecording=false;odysseyStopRequested=false;deviceConnected=false;streamingEnabled=false;
 sleepPending=critical=ota=shortWrite=reconnect=false;micOk=cardOk=openOk=allocOk=true;
 odysseySdBootState=1;
 clockMs=0;reads=micStarts=micStops=closed=sdEnds=collisions=powerActive=powerIdle=detectCalls=0;pendingTask=nullptr;data.clear();opened.clear();
}
void run(){assert(pendingTask);auto fn=pendingTask;pendingTask=nullptr;fn(nullptr);assert(!odysseyRecording);}
int main(){
 reset();collisions=2;odysseyToggleRecording();assert(odysseyRecording && detectCalls==0);run();
 assert(micStarts==1 && micStops==1 && closed==1);assert(powerActive>=1 && powerIdle>=1);
 assert(data.size()==44+3200 && get32(40)==3200 && get32(4)==3236);
 assert(get32(24)==16000 && get32(28)==32000);
 assert(data[44]==0xff && data[45]==0x7f && data[46]==0 && data[47]==0x80);
 assert(opened.find("/synap/odyssey_audio_")==0);
 reset();reconnect=true;odysseyToggleRecording();run();assert(deviceConnected && get32(40)==3200);
 reset();odysseyToggleRecording();odysseyToggleRecording();assert(odysseyStopRequested);run();assert(!micStarts && get32(40)==0);
 reset();cardOk=false;odysseySdBootState=2;odysseyToggleRecording();assert(detectCalls==1 && !odysseyRecording && !pendingTask && data.empty() && !micStarts);
 cardOk=true;odysseyToggleRecording();assert(detectCalls==2 && odysseyRecording && pendingTask);run();assert(get32(40)==3200); // retry succeeds
 reset();shortWrite=true;odysseyToggleRecording();run();assert(get32(40)==8 && odysseySdBootState==2 && closed==1);
 reset();openOk=false;odysseyToggleRecording();run();assert(!micStarts);
 reset();micOk=false;odysseyToggleRecording();run();assert(closed==1 && get32(40)==0);
 reset();allocOk=false;odysseyToggleRecording();assert(!odysseyRecording && !pendingTask && powerActive==1 && powerIdle==1);
 for(int guard=0;guard<5;++guard){reset();switch(guard){case 0:deviceConnected=true;break;case 1:streamingEnabled=true;break;case 2:ota=true;break;case 3:sleepPending=true;break;case 4:critical=true;break;}odysseyToggleRecording();assert(!odysseyRecording && !pendingTask);}
}
