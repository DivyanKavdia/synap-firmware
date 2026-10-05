#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include <algorithm>
constexpr uint32_t SAMPLE_RATE=16000;
void put32le(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8));}
// INSERT IO
std::atomic<uint32_t> connectionGeneration{1};
std::string catalogueBuffer="[]";
char selectedPath[64]{};
bool ready=true;
uint32_t millis(){return 100;}
bool odysseySdReady(){return ready;}
struct OdysseySdGuard {explicit operator bool()const{return true;}};
struct Logger {template<class... T> void printf(const char*,T...) {}} Serial;
bool odysseySdPath(const char* logical,char* full,size_t n) {
  if(strstr(logical,".."))return false;
  return size_t(snprintf(full,n,"/tmp/synap-read%s",logical))<n;
}
enum:uint8_t{OK=0,BAD_COMMAND=2,NO_SD=3,IO_ERROR=7,FILE_UNAVAILABLE=11};
// INSERT PATHS
// INSERT READ
int main(){
  const int cleanup=system("rm -rf /tmp/synap-read");assert(cleanup==0);
  assert(mkdir("/tmp/synap-read",0700)==0&&mkdir("/tmp/synap-read/synap",0700)==0);
  const char* path="/synap/odyssey_audio_test_p0000.wav";
  char full[96];assert(fullPath(path,full,sizeof(full)));
  int fd=open(full,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);
  uint8_t expected[2044]{};odysseyWavHeader(expected,2000);
  for(unsigned i=44;i<sizeof(expected);++i)expected[i]=uint8_t(i);
  assert(odysseyPwriteAll(fd,expected,sizeof(expected),0)&&close(fd)==0);
  uint8_t chunk[480]{};uint32_t total=0;size_t size=0;
  assert(readSelected(path,0,total,chunk,size)==OK&&total==2044&&size==480);
  assert(memcmp(chunk,expected,480)==0&&odysseySdReadFd>=0);
  const int cached=odysseySdReadFd;
  assert(readSelected(path,480,total,chunk,size)==OK&&odysseySdReadFd==cached);
  assert(memcmp(chunk,expected+480,480)==0);
  assert(readSelected(path,0,total,chunk,size)==OK&&memcmp(chunk,expected,480)==0);
  ++connectionGeneration;
  assert(readSelected(path,960,total,chunk,size)==OK&&odysseySdReadConnection==2);
  assert(readSelected(path,1920,total,chunk,size)==OK&&size==124&&odysseySdReadFd==-1);
  assert(memcmp(chunk,expected+1920,124)==0);
  // Direct path reads cannot bypass unfinished-recording validation.
  int journal=odysseyCreateJournal(full);assert(journal>=0&&close(journal)==0);
  assert(readSelected(path,0,total,chunk,size)==FILE_UNAVAILABLE&&odysseySdReadFd==-1);
  assert(odysseyRemoveJournal(full));
  assert(readSelected("/synap/../x.wav",0,total,chunk,size)==BAD_COMMAND);
  ready=false;assert(readSelected(path,0,total,chunk,size)==NO_SD);
  assert(unlink(full)==0);assert(rmdir("/tmp/synap-read/synap")==0&&rmdir("/tmp/synap-read")==0);
}
