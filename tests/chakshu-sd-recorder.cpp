#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
using Clock=std::chrono::steady_clock;
const auto epoch=Clock::now();
uint32_t millis(){return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-epoch).count()*20);}
using TickType_t=uint32_t;
uint32_t xTaskGetTickCount(){return millis();}
uint32_t pdMS_TO_TICKS(uint32_t n){return n;}
void vTaskDelay(uint32_t n){std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1u,n/20)));}
void vTaskDelayUntil(uint32_t* previous,uint32_t n){*previous+=n;if(int32_t(*previous-millis())>0)vTaskDelay(*previous-millis());}
void vTaskDelete(void*){}
constexpr int pdPASS=1,FILE_WRITE=1,PIXFORMAT_JPEG=7;
int created=0,failTask=0,failAllocation=0,allocations=0,cardDelay=1,cameraDelay=8;
bool cardFailed=false,shortWrite=false,micReady=true,cameraReady=true;
unsigned cameraStarts=0,cameraRestores=0;
std::atomic<bool> stop{false};
std::vector<std::thread> tasks;
int xTaskCreatePinnedToCore(void (*fn)(void*),const char*,int,void* arg,int,void*,int){
  if(++created==failTask)return 0;
  tasks.emplace_back([=]{fn(arg);});return pdPASS;
}
void* ps_malloc(size_t n){return ++allocations==failAllocation?nullptr:malloc(n);}
struct MicrophoneGuard {};
bool micRunning=false;unsigned starts=0,stops=0;
bool startMicrophone(){++starts;micRunning=micReady;return micRunning;}
void stopMicrophone(){++stops;micRunning=false;}
struct Microphone {
  uint32_t produced=0;
  size_t readBytes(char* out,size_t n){
    assert(micRunning);std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for(size_t i=0;i<n;++i)out[i]=uint8_t(produced+i);
    produced+=n;return n;
  }
} microphoneI2S;
uint8_t jpeg[96000]={0xff,0xd8};
struct camera_fb_t {int format=PIXFORMAT_JPEG;size_t len=1200;uint8_t* buf=jpeg;struct {int64_t tv_sec=0,tv_usec=0;} timestamp;} frame;
camera_fb_t* esp_camera_fb_get(){
  std::this_thread::sleep_for(std::chrono::milliseconds(cameraDelay)); // normally much slower than each PCM read
  const auto now=millis();frame.timestamp={now/1000,(now%1000)*1000};return cameraReady?&frame:nullptr;
}
void esp_camera_fb_return(camera_fb_t*){}
namespace ChakshuCamera {
 bool ready=true;
 struct VideoProfile {int size;uint16_t width,height;uint8_t fps;};
 bool videoProfile(uint32_t id,VideoProfile& p){if(id>1)return false;p=id==0?VideoProfile{3,1280,720,10}:VideoProfile{2,640,480,20};return true;}
 bool beginVideo(uint32_t id,VideoProfile& p){++cameraStarts;return videoProfile(id,p)&&cameraReady;}
 void endVideo(){++cameraRestores;}
}
struct Contents {std::vector<uint8_t> bytes;};
std::map<std::string,std::shared_ptr<Contents>> files;
uint32_t stopAt=64000;
struct File {
  std::shared_ptr<Contents> content;size_t position=0;std::string path;
  explicit operator bool()const{return bool(content);}
  size_t write(const uint8_t* bytes,size_t n){
    if(cardFailed)return 0;
    const bool pcm=path.find(".wav")!=std::string::npos && n>44;
    if(pcm){std::this_thread::sleep_for(std::chrono::milliseconds(cardDelay));if(shortWrite)n/=2;}
    if(content->bytes.size()<position+n)content->bytes.resize(position+n);
    memcpy(content->bytes.data()+position,bytes,n);position+=n;
    if(pcm && position>=stopAt+44)stop=true;
    return n;
  }
  size_t print(const char* text){return write(reinterpret_cast<const uint8_t*>(text),strlen(text));}
  size_t print(char value){return write(reinterpret_cast<uint8_t*>(&value),1);}
  size_t print(uint32_t value){return print(std::to_string(value).c_str());}
  size_t printf(const char* format,...){char bytes[256];va_list args;va_start(args,format);vsnprintf(bytes,sizeof(bytes),format,args);va_end(args);return print(bytes);}
  bool seek(size_t n){position=n;return true;}
  void flush(){}void close(){content.reset();}
};
struct Storage {
 File open(const char* path,int){if(cardFailed)return {};auto entry=std::make_shared<Contents>();files[path]=entry;return {entry,0,path};}
} SD;
void put32le(uint8_t* p,uint32_t n){for(int i=0;i<4;++i)p[i]=n>>(8*i);}
namespace ChakshuStorage {
 bool ready=true;constexpr uint32_t RESERVE_BYTES=4*1024*1024;uint64_t freeBytes=64*1024*1024;
 File create(char* path,size_t n,const char* ext){snprintf(path,n,"/synap/00000001-00000001.%s",ext);return SD.open(path,FILE_WRITE);}
 // INSERT WAV HEADER
}
namespace ChakshuMedia {
 enum {BAD_COMMAND=2,NO_SD=3,NO_CAMERA=4,NO_MIC=5,NO_SPACE=6,IO_ERROR=7,CAPTURE_ERROR=8};
 struct Snapshot {char path[64]{};uint32_t bytes=0,audioMs=0,frames=0,droppedFrames=0,clipLimitMs=60000;uint16_t width=0,height=0;uint8_t state=0,error=0,progress=0,targetFps=0,videoProfile=0;};
}
// INSERT BUFFERS
// INSERT RECORDER
void progress(const ChakshuMedia::Snapshot& s){assert(s.audioMs<=60000 && s.progress<=99);}
void reset(){
 for(auto& t:tasks)t.join();tasks.clear();files.clear();stop=false;created=allocations=0;
 failTask=failAllocation=0;cardDelay=1;cameraDelay=8;frame.len=1200;cardFailed=shortWrite=false;micReady=cameraReady=true;
 microphoneI2S.produced=0;ChakshuStorage::ready=true;ChakshuStorage::freeBytes=64*1024*1024;
 cameraStarts=cameraRestores=0;
}
ChakshuMedia::Snapshot record(bool video,uint8_t profile=0,uint32_t limit=60000){
 ChakshuMedia::Snapshot s;s.videoProfile=profile;s.clipLimitMs=limit;ChakshuRecorder::record(s,video,stop,progress);
 for(auto& t:tasks)t.join();tasks.clear();assert(!micRunning);return s;
}
void verifyAudio(const ChakshuMedia::Snapshot& s){
 const auto& bytes=files.at("/synap/00000001-00000001.wav")->bytes;
 assert(bytes.size()==44+s.audioMs*32 && !memcmp(bytes.data(),"RIFF",4));
 uint32_t n=0;memcpy(&n,bytes.data()+40,4);assert(n==bytes.size()-44);
 for(size_t i=44;i<bytes.size();++i)assert(bytes[i]==uint8_t(i-44));
}
int main(){
 jpeg[sizeof(jpeg)-2]=0xff;jpeg[sizeof(jpeg)-1]=0xd9;
 auto s=record(true);assert(!s.error && s.state==2 && s.frames>1 && s.audioMs>=2000);verifyAudio(s);
 assert(s.width==1280 && s.height==720 && s.targetFps==10 && cameraStarts==1 && cameraRestores==1);
 auto& index=files.at("/synap/00000001-00000001.json")->bytes;
 assert(index.back()=='}' && std::string(index.begin(),index.end()).find("\"audio\":\"/synap/00000001-00000001.wav\"")!=std::string::npos);
 assert(std::string(index.begin(),index.end()).find("\"width\":1280,\"height\":720,\"targetFps\":10")!=std::string::npos);
 assert(files.at("/synap/00000001-00000001.mjpeg")->bytes.size()==s.frames*frame.len);
 reset();s=record(false);assert(!s.error && s.frames==0 && !cameraStarts && !cameraRestores);verifyAudio(s);
 reset();stopAt=10000000;s=record(true,1,15000);assert(!s.error && s.audioMs==15000 && s.width==640 && s.height==480 && s.targetFps==20 && cameraRestores==1);verifyAudio(s);
 reset();s=record(true,2);assert(s.error==2 && !created && files.empty() && !cameraStarts);
 reset();cardDelay=120;stopAt=1000000;s=record(true);assert(s.error==9 && s.audioMs>0);verifyAudio(s);
 reset();shortWrite=true;s=record(false);assert(s.error==7 && !ChakshuStorage::ready);verifyAudio(s);
 reset();ChakshuStorage::freeBytes=ChakshuStorage::RESERVE_BYTES;s=record(false);assert(s.error==6);
 for(int failed:{1,2}){reset();failTask=failed;s=record(true);assert(s.error==8 && cameraRestores==1);}
 for(int failed:{1,2}){reset();failAllocation=failed;s=record(true);assert(s.error==8 && !created && cameraRestores==1);}
 reset();ChakshuStorage::ready=false;const auto before=starts;s=record(true);assert(s.error==3 && files.empty() && starts==before);
 reset();cardFailed=true;s=record(true);assert(s.error==7 && files.empty());
 reset();cameraReady=false;s=record(true);assert(s.error==8 && cameraRestores==1);
 reset();micReady=false;s=record(true);assert(s.error==5 && !created);
 reset();cameraDelay=1;cardDelay=0;frame.len=sizeof(jpeg);stopAt=10000000;
 s=record(true);assert(!s.error && s.bytes<=32u*1024u*1024u && s.audioMs<60000 && s.droppedFrames>0);verifyAudio(s);
 reset();puts("PASS independent SD capture, PCM integrity, stop drain, overflow, card and worker failures");
}
