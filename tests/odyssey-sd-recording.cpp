#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
constexpr uint32_t SAMPLE_RATE=16000;
constexpr uint16_t SAMPLES_PER_FRAME=800;
constexpr int pdPASS=1;
std::atomic<bool> odysseyRecording{false},odysseyStopRequested{false},deviceConnected{false},streamingEnabled{false};
std::atomic<uint32_t> odysseyRecordingStartedAt{0},odysseyRecordFaultAt{0};
bool sleepPending=false,critical=false,ota=false,micOk=true,cardOk=true,allocOk=true,reconnect=false,pathOk=true,finalizeOnDelay=false;
uint8_t odysseySdBootState=1;
uint32_t clockMs=0,randomCounter=0;
int reads=0,micStarts=0,micStops=0,powerActive=0,powerIdle=0;
void (*pendingTask)(void*)=nullptr;
std::string lastPath;
struct Logger {void println(const char*){} template<class... T> void printf(const char*,T...){} } Serial;
void put32le(uint8_t* p,uint32_t v){for(int i=0;i<4;++i)p[i]=v>>(i*8);}
uint32_t millis(){return clockMs;}
void delay(unsigned ms){
 clockMs+=ms;
 if(finalizeOnDelay && odysseyStopRequested.load()) odysseyRecording=false;
}
uint32_t esp_random(){return ++randomCounter;}
bool otaBusy(){return ota;}
// Native harness records the deferred request; the production transfer worker
// is responsible for remounting away from the touch/control task.
int recoveryRequests=0;
void odysseySdRequestRecovery(){++recoveryRequests;}
int vfsFailures=0;
void odysseySdMarkVfsFailure(){++vfsFailures;odysseySdBootState=2;}
bool batteryCritical(){return critical;}
void applyCpuPowerProfile(bool active){if(active)++powerActive;else ++powerIdle;}
void updateStatusLed(bool=false){}
struct MicrophoneGuard { ~MicrophoneGuard(){} };
bool startMicrophone(){++micStarts;return micOk;}
void stopMicrophone(){++micStops;}
struct Mic {
 size_t readBytes(char* out,size_t n){
  ++reads;clockMs+=1000;
  if(reconnect)deviceConnected=true;
  if(reads>4){odysseyStopRequested=true;return 0;}
  n=std::min(n,size_t(1600));
  for(size_t i=0;i<n;i+=4){uint32_t v=(i%8)?0x80000000u:0x7fff0000u;memcpy(out+i,&v,4);}
  return n;
 }
} microphoneI2S;
struct OdysseySdGuard {
 bool held;
 explicit OdysseySdGuard(unsigned long=~0ul):held(true){}
 explicit operator bool()const{return held;}
};
bool odysseySdReady(){return cardOk && odysseySdBootState==1;}
bool odysseySdPath(const char* logical,char* full,size_t capacity){
 if(!pathOk)return false;
 const int n=snprintf(full,capacity,"/tmp/synap-odyssey-test%s",logical);
 if(n<=0||size_t(n)>=capacity)return false;
 lastPath=full;return true;
}
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){if(!allocOk)return 0;pendingTask=fn;return pdPASS;}
void vTaskDelete(void*){}
// INSERT RECORDER

void reset(){
 odysseyRecording=false;odysseyStopRequested=false;deviceConnected=false;streamingEnabled=false;
 odysseyRecordingStartedAt=0;odysseyRecordFaultAt=0;
 sleepPending=critical=ota=reconnect=finalizeOnDelay=false;micOk=cardOk=allocOk=pathOk=true;odysseySdBootState=1;
 clockMs=randomCounter=0;reads=micStarts=micStops=powerActive=powerIdle=vfsFailures=0;pendingTask=nullptr;lastPath.clear();
 {const int rc=system("rm -rf /tmp/synap-odyssey-test");assert(rc==0);}
 assert(mkdir("/tmp/synap-odyssey-test",0755)==0);
 assert(mkdir("/tmp/synap-odyssey-test/synap",0755)==0);
}
std::vector<uint8_t> load(){
 assert(!lastPath.empty());
 FILE* f=fopen(lastPath.c_str(),"rb");assert(f);
 assert(fseek(f,0,SEEK_END)==0);long n=ftell(f);assert(n>=0);assert(fseek(f,0,SEEK_SET)==0);
 std::vector<uint8_t> data(static_cast<size_t>(n),uint8_t{0});assert(fread(data.data(),1,data.size(),f)==data.size());fclose(f);return data;
}
uint32_t get32(const std::vector<uint8_t>& data,size_t p){
 return uint32_t(data[p])|(uint32_t(data[p+1])<<8)|(uint32_t(data[p+2])<<16)|(uint32_t(data[p+3])<<24);
}
void run(){assert(pendingTask);auto fn=pendingTask;pendingTask=nullptr;fn(nullptr);assert(!odysseyRecording);}

int main(){
 reset();
 // Force two filename collisions to prove exclusive path selection still works.
 FILE* a=fopen("/tmp/synap-odyssey-test/synap/odyssey_audio_00000002_00000001.wav","wb");assert(a);fclose(a);
 FILE* b=fopen("/tmp/synap-odyssey-test/synap/odyssey_audio_00000004_00000003.wav","wb");assert(b);fclose(b);
 odysseyToggleRecording();assert(odysseyRecording);run();
 auto data=load();
 assert(micStarts==1&&micStops==1&&powerActive>=1&&powerIdle>=1);
 assert(data.size()==44+3200&&get32(data,40)==3200&&get32(data,4)==3236);
 assert(get32(data,24)==16000&&get32(data,28)==32000);
 assert(data[44]==0xff&&data[45]==0x7f&&data[46]==0&&data[47]==0x80);
 assert(lastPath.find("/tmp/synap-odyssey-test/synap/odyssey_audio_")==0);

 reset();reconnect=true;odysseyToggleRecording();run();data=load();
 assert(deviceConnected&&get32(data,40)==3200);

 reset();odysseyToggleRecording();odysseyToggleRecording();assert(odysseyStopRequested);run();data=load();
 assert(!micStarts&&get32(data,40)==0);

 reset();cardOk=false;odysseySdBootState=2;odysseyToggleRecording();
 assert(!odysseyRecording&&!pendingTask&&micStarts==0);
 cardOk=true;odysseyToggleRecording();
 assert(!odysseyRecording&&!pendingTask); // no implicit remount
 odysseySdBootState=1;odysseyToggleRecording();assert(odysseyRecording&&pendingTask);run();data=load();
 assert(get32(data,40)==3200);

 reset();pathOk=false;odysseyToggleRecording();run();assert(micStarts==0);
 reset();micOk=false;odysseyToggleRecording();run();data=load();assert(get32(data,40)==0);
 reset();allocOk=false;odysseyToggleRecording();assert(!odysseyRecording&&!pendingTask&&powerActive==1&&powerIdle==1);

 reset();odysseyRecording=true;finalizeOnDelay=true;
 assert(odysseyPrepareForConnectedStreaming(100));
 assert(odysseyStopRequested && !odysseyRecording);

 reset();odysseyRecording=true;
 assert(!odysseyPrepareForConnectedStreaming(20));
 assert(odysseyStopRequested && odysseyRecording && clockMs>=20);

 for(int guard=0;guard<5;++guard){
  reset();
  switch(guard){case 0:deviceConnected=true;break;case 1:streamingEnabled=true;break;case 2:ota=true;break;case 3:sleepPending=true;break;case 4:critical=true;break;}
  odysseyToggleRecording();assert(!odysseyRecording&&!pendingTask);
 }
 {const int rc=system("rm -rf /tmp/synap-odyssey-test");assert(rc==0);}
}
