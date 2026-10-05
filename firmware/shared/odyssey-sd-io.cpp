// C3 file I/O and recoverable recording commit records. All callers own the SD mutex.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// A cached transfer descriptor is always closed under the same storage mutex
// before unmount, format, recording, deletion, or a new transfer identity.
static int odysseySdReadFd=-1;
static char odysseySdReadPath[96]{};
static uint32_t odysseySdReadConnection=0,odysseySdReadAt=0;
static inline bool odysseySdCloseReadLocked() {
  const int fd=odysseySdReadFd;odysseySdReadFd=-1;odysseySdReadPath[0]=0;
  odysseySdReadConnection=0;odysseySdReadAt=0;
  if (fd<0) return true;
  errno=0;
  if (close(fd)==0) return true;
  const int saved=errno?errno:EIO;
  errno=saved;
  return false;
}
static inline uint32_t odysseySdLe32(const uint8_t* p) {
  return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static inline void odysseyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static inline uint32_t odysseySdCrcUpdate(uint32_t crc,const uint8_t* data,size_t size) {
  for (size_t i=0;i<size;++i) {
    crc^=data[i];
    for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);
  }
  return crc;
}
static inline uint32_t odysseySdCrc(const uint8_t* data,size_t size) {
  return ~odysseySdCrcUpdate(0xffffffffu,data,size);
}
static constexpr uint32_t ODYSSEY_WAV_MAX_PCM_BYTES=9600000u;
static constexpr off_t ODYSSEY_INLINE_JOURNAL_OFFSET=
  (44+off_t(ODYSSEY_WAV_MAX_PCM_BYTES)+511)&~off_t(511);
static constexpr size_t ODYSSEY_INLINE_JOURNAL_BYTES=1024u;
static_assert((ODYSSEY_INLINE_JOURNAL_OFFSET&511)==0,
  "inline recovery journal must begin on a physical SD sector boundary");
static constexpr int ODYSSEY_INLINE_JOURNAL=-2;

static inline bool odysseyPwriteAll(int fd,const uint8_t* data,size_t size,off_t offset) {
  // The pinned IDF FatFs pwrite has a zero-write/ENOSPC early return
  // that skips releasing its VFS lock. Own the cursor under the SD mutex.
  const off_t previous=lseek(fd,0,SEEK_CUR);
  if (previous<0 || lseek(fd,offset,SEEK_SET)<0) return false;
  while (size) {
    const ssize_t n=write(fd,data,size);
    if (n<0 && errno==EINTR) continue;
    // Do not issue another filesystem operation after a failed write.
    if (n<=0) { if (!n) errno=ENOSPC;return false; }
    data+=n;size-=size_t(n);
  }
  return lseek(fd,previous,SEEK_SET)>=0;
}
static inline bool odysseyPreadAll(int fd,uint8_t* data,size_t size,off_t offset) {
  while (size) {
    const ssize_t n=pread(fd,data,size,offset);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) { if (!n) errno=EIO;return false; }
    data+=n;size-=size_t(n);offset+=n;
  }
  return true;
}
static inline bool odysseyJournalPath(const char* wav,char* path,size_t capacity) {
  const int n=snprintf(path,capacity,"%s.jrn",wav);
  if (n<0 || size_t(n)>=capacity) { errno=ENAMETOOLONG;return false; }
  return true;
}
static inline bool odysseyRemoveJournal(const char* wav) {
  char path[144];
  return odysseyJournalPath(wav,path,sizeof(path)) && (unlink(path)==0 || errno==ENOENT);
}
// 0=absent, 1=present, -1=filesystem/path error. Legacy sidecars are
// recognized first; new recordings store the two commit sectors inline at the
// reserved tail of the WAV so recording needs only one FAT descriptor.
static inline int odysseyJournalPresence(const char* wav) {
  char path[144];struct stat st{};
  if (!odysseyJournalPath(wav,path,sizeof(path))) return -1;
  errno=0;
  if (stat(path,&st)==0) return 1;
  if (errno!=ENOENT) return -1;
  errno=0;
  if (stat(wav,&st)!=0) {
    if (errno==ENOENT) { errno=0;return 0; }
    return -1;
  }
  errno=0;
  return st.st_size>=ODYSSEY_INLINE_JOURNAL_OFFSET+off_t(ODYSSEY_INLINE_JOURNAL_BYTES)?1:0;
}
static inline bool odysseyJournalAbsent(const char* wav) {
  return odysseyJournalPresence(wav)==0;
}

struct OdysseyWavMeta {
  uint32_t takeHigh=0,takeLow=0,part=0,pcmBytes=0,crc32=0;
};
static inline bool odysseyMetaPath(const char* wav,char* path,size_t capacity) {
  const int n=snprintf(path,capacity,"%s.meta",wav);
  if (n<0 || size_t(n)>=capacity) { errno=ENAMETOOLONG;return false; }
  return true;
}
static inline bool odysseyRemoveMeta(const char* wav) {
  char path[144];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return false;
  errno=0;
  return unlink(path)==0 || errno==ENOENT;
}
// Metadata is an integrity accelerator, not the source of truth. A WAV remains
// recoverable/syncable if this sidecar is absent after sudden power loss.
static inline bool odysseyWriteWavMeta(const char* wav,uint32_t takeHigh,uint32_t takeLow,
    uint32_t part,uint32_t pcmBytes,uint32_t crc32) {
  char path[144],temp[152];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return false;
  const int n=snprintf(temp,sizeof(temp),"%s.tmp",path);
  if (n<0 || size_t(n)>=sizeof(temp)) { errno=ENAMETOOLONG;return false; }

  uint8_t record[32]{};
  memcpy(record,"SYNAPM01",8);
  put32le(record+8,takeHigh);put32le(record+12,takeLow);
  put32le(record+16,part);put32le(record+20,pcmBytes);put32le(record+24,crc32);
  put32le(record+28,odysseySdCrc(record,28));

  errno=0;
  if (unlink(temp)!=0 && errno!=ENOENT) return false;
  errno=0;
  const int fd=open(temp,O_CREAT|O_TRUNC|O_RDWR,0644);
  if (fd<0) return false;
  bool ok=odysseyPwriteAll(fd,record,sizeof(record),0) && fsync(fd)==0;
  int saved=ok?0:(errno?errno:EIO);
  errno=0;
  if (close(fd)!=0) { if (!saved) saved=errno?errno:EIO;ok=false; }
  if (!ok) { (void)unlink(temp);errno=saved?saved:EIO;return false; }

  errno=0;
  if (unlink(path)!=0 && errno!=ENOENT) {
    saved=errno?errno:EIO;(void)unlink(temp);errno=saved;return false;
  }
  errno=0;
  if (rename(temp,path)!=0) {
    saved=errno?errno:EIO;(void)unlink(temp);errno=saved;return false;
  }
  errno=0;
  return true;
}
// 1=valid metadata, 0=absent, -1=invalid metadata, -2=filesystem I/O failure.
static inline int odysseyReadWavMeta(const char* wav,uint32_t& takeHigh,uint32_t& takeLow,
    uint32_t& part,uint32_t& pcmBytes,uint32_t& crc32) {
  char path[144];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return -2;
  errno=0;
  const int fd=open(path,O_RDONLY);
  if (fd<0) {
    if (errno==ENOENT) { errno=0;return 0; }
    return -2;
  }
  uint8_t record[32]{};
  struct stat st{};
  bool ok=fstat(fd,&st)==0 && st.st_size==off_t(sizeof(record)) &&
    odysseyPreadAll(fd,record,sizeof(record),0);
  int saved=ok?0:(errno?errno:EIO);
  errno=0;
  if (close(fd)!=0) { if (!saved) saved=errno?errno:EIO;ok=false; }
  if (!ok) { errno=saved?saved:EIO;return -2; }
  if (memcmp(record,"SYNAPM01",8)!=0 ||
      odysseySdLe32(record+28)!=odysseySdCrc(record,28)) {
    errno=0;return -1;
  }
  takeHigh=odysseySdLe32(record+8);takeLow=odysseySdLe32(record+12);
  part=odysseySdLe32(record+16);pcmBytes=odysseySdLe32(record+20);
  crc32=odysseySdLe32(record+24);
  errno=0;
  return 1;
}
static inline int odysseyCreateJournal(const char* wav) {
  char path[144];
  if (!odysseyJournalPath(wav,path,sizeof(path))) return -1;
  const int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0644);
  if (fd<0) return -1;
  // Reserve both sectors before capture starts; checkpoints never grow this file.
  uint8_t empty[512]{};
  if (!odysseyPwriteAll(fd,empty,sizeof(empty),0) ||
      !odysseyPwriteAll(fd,empty,sizeof(empty),512) || fsync(fd)!=0) {
    const int saved=errno;close(fd);errno=saved;return -1;
  }
  return fd;
}
static inline bool odysseyJournalCommit(int wavFd,int journal,const char* path,
    uint32_t bytes,uint32_t& sequence) {
  // Sync PCM first. Only then advertise its length in the alternate commit
  // sector. New C3 recordings use the WAV descriptor itself; legacy sidecar
  // descriptors remain supported for recovery/fixture compatibility.
  if (fsync(wavFd)!=0) return false;
  uint8_t record[512]{};
  memcpy(record,"SYNAPJ01",8);
  const uint32_t next=sequence+1;
  put32le(record+8,next);put32le(record+12,bytes);
  put32le(record+16,odysseySdCrc(reinterpret_cast<const uint8_t*>(path),strlen(path)));
  put32le(record+20,SAMPLE_RATE);
  const size_t prefix=std::min(size_t(bytes),size_t(468));
  if (prefix && !odysseyPreadAll(wavFd,record+24,prefix,44)) return false;
  put32le(record+508,odysseySdCrc(record,508));

  const bool inlineJournal=journal==ODYSSEY_INLINE_JOURNAL;
  const int target=inlineJournal?wavFd:journal;
  if (target<0) { errno=EBADF;return false; }
  const off_t base=inlineJournal?ODYSSEY_INLINE_JOURNAL_OFFSET:0;
  if (!odysseyPwriteAll(target,record,sizeof(record),
        base+off_t((next-1u)&1u)*512) || fsync(target)!=0)
    return false;
  sequence=next;
  return true;
}
// Returns 1 for a valid commit, 0 for no journal, -1 for invalid journal, -2 for I/O failure.
static inline int odysseyReadJournalSlots(int fd,off_t base,const char* path,
    uint32_t& bytes,uint8_t* firstPcm) {
  uint32_t newest=0;
  const uint32_t identity=odysseySdCrc(reinterpret_cast<const uint8_t*>(path),strlen(path));
  for (unsigned slot=0;slot<2;++slot) {
    uint8_t record[512]{};
    if (!odysseyPreadAll(fd,record,sizeof(record),base+off_t(slot)*512)) return -2;
    const uint32_t seq=odysseySdLe32(record+8),size=odysseySdLe32(record+12);
    if (memcmp(record,"SYNAPJ01",8)==0 && seq>newest &&
        size<=ODYSSEY_WAV_MAX_PCM_BYTES && !(size&1u) &&
        odysseySdLe32(record+16)==identity && odysseySdLe32(record+20)==SAMPLE_RATE &&
        odysseySdLe32(record+508)==odysseySdCrc(record,508)) {
      newest=seq;bytes=size;memcpy(firstPcm,record+24,468);
    }
  }
  errno=0;
  return newest?1:-1;
}
static inline int odysseyJournalRead(const char* path,uint32_t& bytes,uint8_t* firstPcm) {
  // First support the legacy .jrn sidecar so recordings made by older firmware
  // remain recoverable after OTA.
  char journalPath[144];
  if (!odysseyJournalPath(path,journalPath,sizeof(journalPath))) return -2;
  errno=0;
  int fd=open(journalPath,O_RDONLY);
  if (fd>=0) {
    struct stat st{};
    if (fstat(fd,&st)!=0) {
      const int saved=errno?errno:EIO;(void)close(fd);errno=saved;return -2;
    }
    int result=-1;
    if (st.st_size==off_t(ODYSSEY_INLINE_JOURNAL_BYTES))
      result=odysseyReadJournalSlots(fd,0,path,bytes,firstPcm);
    const int saved=errno;
    if (close(fd)!=0 && result>=0) return -2;
    errno=saved;
    return result;
  }
  if (errno!=ENOENT) return -2;

  // New recordings keep recovery commits in the reservation tail of the WAV.
  errno=0;
  fd=open(path,O_RDONLY);
  if (fd<0) return errno==ENOENT?0:-2;
  struct stat st{};
  if (fstat(fd,&st)!=0) {
    const int saved=errno?errno:EIO;(void)close(fd);errno=saved;return -2;
  }
  if (st.st_size<ODYSSEY_INLINE_JOURNAL_OFFSET+off_t(ODYSSEY_INLINE_JOURNAL_BYTES)) {
    const int rc=close(fd);
    if (rc!=0) return -2;
    errno=0;
    return 0;
  }
  const int result=odysseyReadJournalSlots(fd,ODYSSEY_INLINE_JOURNAL_OFFSET,path,bytes,firstPcm);
  const int saved=errno;
  if (close(fd)!=0 && result>=0) return -2;
  errno=saved;
  return result;
}
static inline bool odysseyWavValid(const uint8_t* h,uint32_t& bytes) {
  bytes=odysseySdLe32(h+40);
  uint8_t expected[44];odysseyWavHeader(expected,bytes);
  return bytes<=ODYSSEY_WAV_MAX_PCM_BYTES && !(bytes&1u) && memcmp(h,expected,44)==0;
}
// Retryable recovery: retain the journal until header, length and close all succeed.
static inline bool odysseyRecoverWav(const char* path) {
  errno=0;
  struct stat st{};
  if (stat(path,&st)!=0) return false;
  uint32_t bytes=0;
  uint8_t firstSector[512]{};
  const int journal=odysseyJournalRead(path,bytes,firstSector+44);
  if (journal==-2) return false;
  if (journal==-1) {
    // Preserve questionable data and its journal for manual recovery; this is
    // a content-integrity condition, not evidence that the volume itself failed.
    errno=0;
    return true;
  }
  errno=0;
  const int fd=open(path,O_RDWR);
  if (fd<0) return false;
  uint8_t header[44]{};
  bool ok=true,valid=true;
  int savedError=0;
  if (!journal) {
    if (st.st_size<44) valid=false;
    else if (!odysseyPreadAll(fd,header,sizeof(header),0)) {
      savedError=errno?errno:EIO;ok=false;
    } else valid=odysseyWavValid(header,bytes);
  }
  const uint64_t committed=44ull+bytes;
  if (committed>uint64_t(st.st_size)) valid=false;
  if (ok && valid && (journal || committed!=uint64_t(st.st_size))) {
    odysseyWavHeader(header,bytes);
    memcpy(firstSector,header,44);
    const size_t repairBytes=journal?44+std::min(size_t(bytes),size_t(468)):44;
    if (!odysseyPwriteAll(fd,firstSector,repairBytes,0) || fsync(fd)!=0 ||
        ftruncate(fd,off_t(committed))!=0 || fsync(fd)!=0) {
      savedError=errno?errno:EIO;ok=false;
    }
  }
  if (close(fd)!=0) {
    if (!savedError) savedError=errno?errno:EIO;
    ok=false;
  }
  if (!ok) { errno=savedError?savedError:EIO;return false; }
  if (!valid) { errno=0;return true; }
  if (journal && !odysseyRemoveJournal(path)) return false;
  if (bytes) { errno=0;return true; }
  errno=0;
  if (unlink(path)==0 || errno==ENOENT) { errno=0;return true; }
  return false;
}
#endif
