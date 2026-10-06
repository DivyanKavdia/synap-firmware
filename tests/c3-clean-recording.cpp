#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <string>
constexpr uint32_t SAMPLE_RATE=16000,SAMPLES_PER_FRAME=800,ODYSSEY_SD_FLUSH_MS=5000;
constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=512;
constexpr int pdPASS=1;
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0;
const char* esp_err_to_name(esp_err_t){return "ESP_OK";}
esp_err_t odysseyNativeLastError=ESP_OK;
uint8_t odysseyCleanWriteBuffer[4096];
std::atomic<bool> odysseyRecording{false},odysseyStopRequested{false},odysseyCaptureActive{false},odysseySdRecoveryActive{false},deviceConnected{false},streamingEnabled{false};
std::atomic<uint32_t> odysseyRecordingStartedAt{0},odysseyRecordFaultAt{0},odysseySdSleepGuardUntil{0};
std::atomic<uint8_t> odysseySdBootState{0},odysseySdProbeStage{0};
uint32_t disconnectedAt=0,clockMs=1,randomId=0;
bool mountOK=true,micOK=true,cancelOnMount=false,lowBattery=false,sleepPending=false,ota=false,connectDuringTake=false;
int mounts=0,unmounts=0,micStarts=0,micStops=0,purple=0,reads=0,stopAfter=4;
int syncs=0,failSync=0,writes=0,failWrite=0;size_t maxWrite=0;bool failClose=false,failRename=false,shortWrites=false;
void (*task)(void*)=nullptr;
uint32_t millis(){return clockMs;}
void delay(uint32_t n){clockMs+=n;}
void applyCpuPowerProfile(bool){}
bool batteryCritical(){return lowBattery;}
bool otaBusy(){return ota;}
uint32_t esp_random(){return ++randomId;}
void put32le(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
struct Logger { void println(const char*){} template<class...T> void printf(const char*,T...){} } Serial;
struct MicrophoneGuard{MicrophoneGuard(){} ~MicrophoneGuard(){}};
bool startMicrophone(){++micStarts;return micOK;}
void stopMicrophone(){++micStops;}
void updateStatusLed(bool){if(odysseyCaptureActive){++purple;assert(syncs>=2);}}
bool odysseyNativeMountForTake(){++mounts;if(cancelOnMount)odysseyStopRequested=true;return mountOK;}
void odysseyNativeUnmount(bool=true){++unmounts;}
void odysseyToggleRecording();
struct Mic {
 size_t readBytes(char* p,size_t n){
  ++reads;clockMs+=50;
  if(connectDuringTake)deviceConnected=true;
  if(reads>stopAfter){odysseyToggleRecording();return 0;}
  for(size_t i=0;i<n;i+=4){int32_t v=0x12340000;memcpy(p+i,&v,4);}
  return n;
 }
} microphoneI2S;
int xTaskCreate(void(*fn)(void*),const char*,int,void*,int,void*){task=fn;return pdPASS;}
void vTaskDelete(void*){}
ssize_t checkedWrite(int fd,const void* p,size_t n){++writes;maxWrite=std::max(maxWrite,n);if(writes==failWrite){errno=EIO;return -1;}return ::write(fd,p,shortWrites?std::min(n,size_t(37)):n);}
int checkedSync(int fd){++syncs;if(syncs==failSync){errno=EIO;return -1;}return ::fsync(fd);}
int checkedClose(int fd){int r=::close(fd);if(failClose){errno=EIO;return -1;}return r;}
int checkedRename(const char* a,const char* b){if(failRename){errno=EIO;return -1;}return ::rename(a,b);}
#define write checkedWrite
#define fsync checkedSync
#define close checkedClose
#define rename checkedRename
// INSERT PRODUCTION
#undef write
#undef fsync
#undef close
#undef rename
void reset(){
 for(const auto& f:std::filesystem::directory_iterator("."))std::filesystem::remove(f.path());
 odysseyRecording=false;odysseyStopRequested=false;odysseyCaptureActive=false;odysseySdRecoveryActive=false;deviceConnected=false;streamingEnabled=false;
 odysseySdBootState=0;odysseySdProbeStage=0;
 mountOK=micOK=true;cancelOnMount=lowBattery=sleepPending=ota=connectDuringTake=false;
 mounts=unmounts=micStarts=micStops=purple=reads=syncs=writes=0;stopAfter=4;
 failSync=failWrite=0;maxWrite=0;failClose=failRename=shortWrites=false;clockMs=1;randomId=0;task=nullptr;
}
void run(){assert(task);task(nullptr);assert(!odysseyRecording&&!odysseyCaptureActive);}
std::vector<std::string> files(const std::string& extension){
 std::vector<std::string> out;for(const auto& f:std::filesystem::directory_iterator("."))if(f.path().extension()==extension)out.push_back(f.path().string());return out;
}
void verify(size_t frames){
 auto wav=files(".wav");assert(wav.size()==1&&files(".part").empty());
 FILE* f=fopen(wav[0].c_str(),"rb");assert(f);
 std::vector<uint8_t> bytes(44+frames*1600);assert(fread(bytes.data(),1,bytes.size(),f)==bytes.size());assert(fgetc(f)==EOF);fclose(f);
 uint8_t h[44];odysseyCleanWavHeader(h,frames*1600);assert(memcmp(bytes.data(),h,44)==0);
 for(size_t i=44;i<bytes.size();i+=2){assert(bytes[i]==0x34&&bytes[i+1]==0x12);}
 assert(micStarts==1&&micStops==1&&unmounts==1&&purple>0);assert(maxWrite<=512);
}
int main(){
 char dir[]="/tmp/sc-XXXXXX";assert(mkdtemp(dir));assert(chdir(dir)==0);
 reset();odysseyToggleRecording();run();verify(4);
 randomId=0;reads=0;task=nullptr;odysseyToggleRecording();run();assert(files(".wav").size()==2);
 reset();shortWrites=true;stopAfter=110;odysseyToggleRecording();run();verify(110);assert(syncs>=5);
 reset();connectDuringTake=true;odysseyToggleRecording();run();verify(4);
 reset();odysseyToggleRecording();odysseyToggleRecording();run();assert(mounts==0&&micStarts==0&&files(".wav").empty());
 reset();cancelOnMount=true;odysseyToggleRecording();run();assert(micStarts==0&&unmounts==1&&files(".part").empty());
 reset();lowBattery=true;odysseyToggleRecording();assert(!task);
 reset();mountOK=false;odysseyToggleRecording();run();assert(!micStarts&&files(".part").empty());
 reset();failSync=1;odysseyToggleRecording();run();assert(!micStarts&&!purple&&files(".wav").empty()&&files(".part").size()==1);assert(odysseySdBootState==2&&odysseySdProbeStage==5);
 reset();failSync=2;odysseyToggleRecording();run();assert(!purple&&files(".wav").empty()&&files(".part").size()==1);assert(odysseySdBootState==2&&odysseySdProbeStage==5);
 reset();failWrite=5;odysseyToggleRecording();run();assert(purple>0&&files(".wav").empty()&&files(".part").size()==1);assert(odysseySdBootState==2&&odysseySdProbeStage==4);
 reset();failClose=true;odysseyToggleRecording();run();assert(files(".wav").empty()&&files(".part").size()==1);
 reset();failRename=true;odysseyToggleRecording();run();assert(files(".wav").empty()&&files(".part").size()==1);
 reset();assert(chdir("/tmp")==0);assert(rmdir(dir)==0);
}
