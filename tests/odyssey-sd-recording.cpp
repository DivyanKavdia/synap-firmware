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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
constexpr uint32_t SAMPLE_RATE=16000;
constexpr uint16_t SAMPLES_PER_FRAME=800;
constexpr int pdPASS=1;
std::atomic<bool> odysseyRecording{false},odysseyStopRequested{false},deviceConnected{false},streamingEnabled{false};
std::atomic<bool> odysseySdRecoveryActive{false},odysseyCaptureActive{false};
std::atomic<uint32_t> odysseyRecordingStartedAt{0},odysseyRecordFaultAt{0};
std::atomic<uint32_t> odysseySdSleepGuardUntil{0};
uint32_t disconnectedAt=0;
bool sleepPending=false,critical=false,ota=false,micOk=true,cardOk=true,allocOk=true,reconnect=false,pathOk=true,finalizeOnDelay=false,recoverOk=true;
int stopAfterReads=4;
uint64_t freeBytes=64ull*1024ull*1024ull;
uint8_t odysseySdBootState=1;
uint32_t clockMs=0,randomCounter=0;
int reads=0,micStarts=0,micStops=0,powerActive=0,powerIdle=0;
int preallocationCalls=0;
std::vector<uint64_t> preallocatedSizes;
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
int recoverCalls=0;
bool odysseyRecoverSdCard(const char*) {
 ++recoverCalls;
 if(!recoverOk)return false;
 cardOk=true;odysseySdBootState=1;return true;
}
uint64_t odysseySdFreeBytesLocked(){return freeBytes;}
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
  ++reads;assert(odysseyCaptureActive.load());assert(!odysseySdRecoveryActive.load());clockMs+=1000;
  if(reconnect)deviceConnected=true;
  if(reads>stopAfterReads){odysseyStopRequested=true;return 0;}
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
bool odysseySdPreallocateFile(const char* full,uint64_t size){
 int fd=open(full,O_CREAT|O_EXCL|O_RDWR,0644);if(fd<0)return false;
 const bool ok=ftruncate(fd,off_t(size))==0;close(fd);
 if(ok){++preallocationCalls;preallocatedSizes.push_back(size);}return ok;
}
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){if(!allocOk)return 0;pendingTask=fn;return pdPASS;}
void vTaskDelete(void*){}
void odysseySaveRecordFailure(uint8_t,int,uint32_t) {}
// INSERT RECORDER

void reset(){
 odysseyRecording=false;odysseyStopRequested=false;deviceConnected=false;streamingEnabled=false;odysseySdRecoveryActive=false;odysseyCaptureActive=false;
 odysseyRecordingStartedAt=0;odysseyRecordFaultAt=0;
 odysseySdSleepGuardUntil=0;disconnectedAt=0;
 sleepPending=critical=ota=reconnect=finalizeOnDelay=false;micOk=cardOk=allocOk=pathOk=recoverOk=true;odysseySdBootState=1;
 freeBytes=64ull*1024ull*1024ull;
 clockMs=randomCounter=0;reads=micStarts=micStops=powerActive=powerIdle=vfsFailures=recoverCalls=0;preallocationCalls=0;preallocatedSizes.clear();stopAfterReads=4;pendingTask=nullptr;lastPath.clear();
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
 FILE* a=fopen("/tmp/synap-odyssey-test/synap/odyssey_audio_00000001_00000002_p0000.wav","wb");assert(a);fclose(a);
 FILE* b=fopen("/tmp/synap-odyssey-test/synap/odyssey_audio_00000003_00000004_p0000.wav","wb");assert(b);fclose(b);
 odysseyToggleRecording();assert(odysseyRecording&&odysseySdRecoveryActive&&!odysseyCaptureActive);run();
 auto data=load();
 assert(micStarts==1&&micStops==1&&powerActive>=1&&powerIdle>=1);
 assert(data.size()==44+3200&&get32(data,40)==3200&&get32(data,4)==3236);
 assert(get32(data,24)==16000&&get32(data,28)==32000);
 assert(data[44]==0xff&&data[45]==0x7f&&data[46]==0&&data[47]==0x80);
 assert(lastPath.find("/tmp/synap-odyssey-test/synap/odyssey_audio_00000005_00000006_p0000.wav")==0);
 assert(preallocationCalls==1&&preallocatedSizes[0]==9601536);
 OdysseyWavMeta firstMeta{};
 assert(odysseyReadWavMeta(lastPath.c_str(),firstMeta.takeHigh,firstMeta.takeLow,
   firstMeta.part,firstMeta.pcmBytes,firstMeta.crc32)==1);
 assert(firstMeta.takeHigh==5&&firstMeta.takeLow==6&&firstMeta.part==0&&firstMeta.pcmBytes==3200);
 assert(firstMeta.crc32==odysseySdCrc(data.data()+44,data.size()-44));

 // Exercise routine sector-aware draining and multiple 15-second checkpoints,
 // not only the short final-flush path.
 reset();stopAfterReads=40;odysseyToggleRecording();run();data=load();
 assert(data.size()==44+32000&&get32(data,40)==32000&&get32(data,4)==32036);
 assert(vfsFailures==0);
 assert(odysseySdRecordFailureStage()==0);
 assert(odysseySdRecordLastBytes()==32000);

 // A five-minute extent rolls into a consecutive WAV part with exact sample ordering in this non-timed fixture and
 // each part is trimmed from its reservation before it is exposed to sync.
 reset();stopAfterReads=12002;odysseyToggleRecording();run();
 char firstPath[128];snprintf(firstPath,sizeof(firstPath),"/tmp/synap-odyssey-test/synap/odyssey_audio_00000001_00000002_p0000.wav");
 struct stat firstStat{};assert(stat(firstPath,&firstStat)==0&&firstStat.st_size==9600044);
 assert(lastPath.find("_p0001.wav")!=std::string::npos);
 data=load();assert(data.size()==44+1600&&get32(data,40)==1600);
 assert(preallocationCalls==2&&preallocatedSizes[0]==9601536&&preallocatedSizes[1]==9601536);
 assert(odysseySdRecordLastBytes()==9601600);

 reset();reconnect=true;odysseyToggleRecording();run();data=load();
 assert(deviceConnected&&get32(data,40)==3200);

 reset();odysseyToggleRecording();odysseyToggleRecording();assert(odysseyStopRequested);run();
 struct stat emptyStat{};assert(!micStarts&&stat(lastPath.c_str(),&emptyStat)!=0&&errno==ENOENT);

 reset();cardOk=false;odysseySdBootState=2;odysseyToggleRecording();
 assert(odysseyRecording&&pendingTask&&odysseySdRecoveryActive);
 run();data=load();
 assert(recoverCalls==1&&micStarts==1&&get32(data,40)==3200);

 reset();cardOk=false;odysseySdBootState=2;recoverOk=false;odysseyToggleRecording();
 assert(odysseyRecording&&pendingTask);run();
 assert(recoverCalls==1&&micStarts==0&&!odysseyRecording&&odysseySdRecordFailureStage()==1);

 reset();freeBytes=5ull*1024ull*1024ull;odysseyToggleRecording();run();
 assert(micStarts==0&&odysseySdRecordFailureStage()==31&&vfsFailures==0&&odysseySdBootState==1);

 reset();pathOk=false;odysseyToggleRecording();run();
 assert(recoverCalls==1&&odysseySdBootState==1); // failed take autonomously prepares the next one

 reset();micOk=false;odysseyToggleRecording();run();
 assert(stat(lastPath.c_str(),&emptyStat)!=0&&errno==ENOENT&&odysseySdRecordFailureStage()==4);
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
