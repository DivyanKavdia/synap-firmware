#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
constexpr uint32_t SAMPLE_RATE=16000;
void put32le(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
static int interrupted=0,syncFailures=0;
static bool noSpace=false;
static size_t writeLimit=0;
static ssize_t testWrite(int fd,const void* p,size_t n) {
  if (noSpace) return 0;
  if (interrupted) { --interrupted;errno=EINTR;return -1; }
  return ::write(fd,p,writeLimit?std::min(n,writeLimit):n);
}
static int testSync(int fd) {
  if (syncFailures) { --syncFailures;errno=EIO;return -1; }
  return ::fsync(fd);
}
#define write testWrite
#define fsync testSync
// INSERT IO
#undef write
#undef fsync
const char* path="/tmp/synap-journal-test.wav";
static int createWav() {
  unlink(path);assert(odysseyRemoveJournal(path));
  int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);
  assert(ftruncate(fd,9600044)==0);
  uint8_t header[44];odysseyWavHeader(header,0);
  assert(odysseyPwriteAll(fd,header,44,0));
  uint8_t pcm[16384];memset(pcm,0x6a,sizeof(pcm));
  assert(odysseyPwriteAll(fd,pcm,sizeof(pcm),44));
  return fd;
}
static void verify(uint32_t bytes) {
  int fd=open(path,O_RDONLY);assert(fd>=0);
  struct stat st{};assert(fstat(fd,&st)==0 && st.st_size==44+bytes);
  uint8_t header[44];assert(odysseyPreadAll(fd,header,44,0));
  uint32_t count=0;assert(odysseyWavValid(header,count)&&count==bytes);
  uint8_t pcm[16384];assert(odysseyPreadAll(fd,pcm,bytes,44));
  for(uint32_t i=0;i<bytes;++i)assert(pcm[i]==0x6a);
  assert(close(fd)==0);assert(odysseyJournalAbsent(path));
}
int main() {
  int fd=createWav();int journal=odysseyCreateJournal(path);assert(journal>=0);
  uint32_t seq=0;
  const off_t cursor=lseek(fd,99,SEEK_SET);assert(cursor==99);
  uint8_t sample=0x6a;assert(odysseyPwriteAll(fd,&sample,1,44));
  assert(lseek(fd,0,SEEK_CUR)==99);
  noSpace=true;assert(!odysseyPwriteAll(fd,&sample,1,44)&&errno==ENOSPC);
  noSpace=false;assert(odysseyPwriteAll(fd,&sample,1,44));
  interrupted=2;writeLimit=31;
  assert(odysseyJournalCommit(fd,journal,path,8192,seq));assert(seq==1);
  writeLimit=0;
  assert(odysseyJournalCommit(fd,journal,path,16384,seq));assert(seq==2);
  // Corrupt the newer commit and the whole first WAV sector. Recovery uses the older CRC-valid slot.
  uint8_t bad[512]{};assert(pwrite(fd,bad,512,0)==512);
  assert(pwrite(journal,bad,1,512+508)==1);
  assert(close(fd)==0&&close(journal)==0);
  assert(odysseyRecoverWav(path));verify(8192);
  assert(odysseyRecoverWav(path));verify(8192); // idempotent

  fd=createWav();journal=odysseyCreateJournal(path);seq=0;
  assert(odysseyJournalCommit(fd,journal,path,8192,seq));
  syncFailures=1;assert(!odysseyJournalCommit(fd,journal,path,16384,seq));assert(seq==1);
  assert(close(fd)==0&&close(journal)==0);
  assert(odysseyRecoverWav(path));verify(8192);

  // Unknown or torn journals must preserve reservations, never publish fake PCM.
  fd=createWav();journal=odysseyCreateJournal(path);assert(journal>=0);
  assert(close(fd)==0&&close(journal)==0);
  assert(odysseyRecoverWav(path));assert(!odysseyJournalAbsent(path));
  struct stat st{};assert(stat(path,&st)==0&&st.st_size==9600044);
  char journalPath[144];assert(odysseyJournalPath(path,journalPath,sizeof(journalPath)));
  assert(truncate(journalPath,100)==0);assert(odysseyRecoverWav(path));
  assert(stat(path,&st)==0&&st.st_size==9600044);

  // Support an old reserved part whose RIFF length was its only checkpoint.
  fd=createWav();uint8_t header[44];odysseyWavHeader(header,8192);
  assert(pwrite(fd,header,44,0)==44&&close(fd)==0);
  assert(odysseyRecoverWav(path));verify(8192);

  // Cached reads must not leave a descriptor behind when storage is released.
  odysseySdReadFd=open(path,O_RDONLY);assert(odysseySdReadFd>=0);
  const int cached=odysseySdReadFd;odysseySdReadConnection=42;odysseySdReadAt=100;
  assert(odysseySdCloseReadLocked());assert(odysseySdReadFd==-1);
  assert(fcntl(cached,F_GETFD)==-1&&errno==EBADF);
  assert(odysseySdCloseReadLocked());
  assert(unlink(path)==0);assert(odysseyRemoveJournal(path));
}
