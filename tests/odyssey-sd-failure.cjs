'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('recording failure persists as one complete record and restores across boot',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-detect.cpp','utf8');
 const code=source.slice(source.indexOf('static uint32_t odysseyStoredStage'),source.indexOf('bool odysseySdPreallocateFile'));
 nativeTest(`#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define SYNAP_BUILD 1724
static std::vector<uint8_t> saved;
struct Preferences {
 bool begin(const char*,bool){return true;}
 size_t getBytesLength(const char*){return saved.size();}
 size_t getBytes(const char*,void* out,size_t n){memcpy(out,saved.data(),n);return n;}
 size_t putBytes(const char*,const void* in,size_t n){auto p=static_cast<const uint8_t*>(in);saved.assign(p,p+n);return n;}
 void end(){}
};
struct {void println(const char*){}} Serial;
${code}
int main(){
 odysseyLoadRecordFailure();assert(odysseyStoredStage==0);
 odysseySaveRecordFailure(30,5,1234);
 odysseyStoredStage=0;odysseyStoredErrno=0;odysseyStoredBytes=0;odysseyStoredBuild=0;
 odysseyLoadRecordFailure();assert(odysseyStoredStage==30&&odysseyStoredErrno==5&&odysseyStoredBytes==1234&&odysseyStoredBuild==1724);
 saved.resize(3);odysseyStoredStage=0;odysseyLoadRecordFailure();assert(odysseyStoredStage==0);
}`);
});
test('preallocation falls back only on allocation denial for an empty file',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-detect.cpp','utf8');
 const code=source.slice(source.indexOf('bool odysseySdPreallocateFile'),source.indexOf('static bool odysseySdReleaseLocked'));
 nativeTest(`#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
using esp_err_t=int;
constexpr int ESP_OK=0;
const char* ODYSSEY_SD_MOUNT_POINT="/tmp";
int odysseySdLastMountError=0,mode=0;
const char* esp_err_to_name(int){return "test";}
struct {template<class... T> void printf(const char*,T...) {}} Serial;
int esp_vfs_fat_create_contiguous_file(const char*,const char* path,uint64_t,bool){
 if(mode==2){int fd=open(path,O_RDWR);assert(fd>=0);assert(ftruncate(fd,44)==0);close(fd);}
 errno=mode==1?EIO:EACCES;return -1;
}
${code}
int main(){
 const char* path="/tmp/synap-preallocation-test.wav";unlink(path);
 assert(odysseySdPreallocateFile(path,9600044));unlink(path);
 mode=1;assert(!odysseySdPreallocateFile(path,9600044)&&errno==EIO);unlink(path);
 mode=2;assert(!odysseySdPreallocateFile(path,9600044)&&errno==EACCES);unlink(path);
}`);
});
