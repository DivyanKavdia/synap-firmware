'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const contract=fs.readFileSync('firmware/xiao-sense/model-contract.cpp','utf8');
test('model upload resumes at its actual offset, verifies before install, and rejects corruption and stale owners',()=>{
  const body=`#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
${contract}
using namespace ChakshuModel;
struct Storage:Backend {
  Error start=OK,finishResult=OK;bool good=true;int begins=0,finishes=0,aborts=0;std::vector<uint8_t> bytes;
  Error begin()override{++begins;bytes.clear();return start;}
  bool write(const uint8_t* p,size_t n)override{if(!good)return false;bytes.insert(bytes.end(),p,p+n);return true;}
  Error finish()override{++finishes;assert(bytes.size()==MODEL_BYTES);return finishResult;}
  void abort()override{++aborts;}
};
void put(uint8_t* p,uint32_t v){for(int i=0;i<4;i++)p[i]=uint8_t(v>>(i*8));}
void packet(Upload& u,uint8_t op,uint32_t owner=1,uint32_t id=42,uint32_t at=0,size_t n=0){
  uint8_t p[489]{};p[0]=op;put(p+1,id);put(p+5,at);for(size_t i=0;i<n;i++)p[9+i]=uint8_t(at+i);
  u.packet(p,n?9+n:5,owner,1000);
}
void fill(Upload& u,uint32_t owner=1){while(u.offset<MODEL_BYTES)packet(u,2,owner,42,u.offset,std::min(size_t(480),MODEL_BYTES-u.offset));}
int main(){
 Storage b;Upload u(b);packet(u,1);assert(u.state==RECEIVING&&b.begins==1);
 packet(u,1);assert(b.begins==1);packet(u,2,1,42,0,480);packet(u,2,1,42,0,480);assert(u.offset==480);
 packet(u,2,2,42,480,100);assert(u.offset==480);packet(u,6,2,99);assert(u.offset==480);
 packet(u,6,2);packet(u,2,1,42,480,100);assert(u.offset==480);
 fill(u,2);assert(b.bytes.size()==MODEL_BYTES);packet(u,3,2);assert(u.state==VERIFYING&&b.finishes==0);
 u.verify();assert(u.state==INSTALLED&&b.finishes==1);
 for(size_t i=0;i<b.bytes.size();i++)assert(b.bytes[i]==uint8_t(i));
 packet(u,1);packet(u,2,1,42,10,10);assert(u.state==FAILED&&u.error==BAD_OFFSET);
 packet(u,1);packet(u,3);assert(u.state==FAILED&&b.finishes==1);
 packet(u,1);b.good=false;packet(u,2,1,42,0,10);assert(u.error==IO_ERROR&&u.offset==0);b.good=true;
 packet(u,1);fill(u);b.finishResult=HASH_MISMATCH;packet(u,3);u.verify();assert(u.state==FAILED&&u.error==HASH_MISMATCH);
 packet(u,1);packet(u,5);assert(!u.busy()&&u.error==CANCELLED);
 packet(u,1);u.tick(901001);assert(u.error==TIMED_OUT);
 b.start=NO_SPACE;packet(u,1);assert(!u.busy()&&u.error==NO_SPACE);
 b.start=BUSY;packet(u,1);assert(!u.busy()&&u.error==BUSY);
 b.start=OK;packet(u,1);uint8_t p[5]={6,42,0,0,0};u.packet(p,5,2,UINT32_MAX-100);u.tick(100);assert(u.busy());
 puts("PASS model transfer lifecycle");
}`;
  assert.match(nativeTest(body),/PASS model transfer lifecycle/);
});
test('SD backend preserves recordings and old model on bad hash, IO errors, cancellation and interrupted replacement',()=>{
  const source=fs.readFileSync('firmware/xiao-sense/model-upload.cpp','utf8');
  const backend=source.slice(source.indexOf('namespace ChakshuModel'),source.indexOf('SdBackend backend;'))+'}';
  const body=`#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
${contract}
constexpr int FILE_WRITE=1,FILE_READ=0;
bool updating=false;bool otaBusy(){return updating;}
std::atomic<bool> streamingEnabled{false};bool remoteStandby=false;
namespace ChakshuMedia {std::atomic<bool> busy{false};}
namespace ChakshuStorage {bool ready=true;uint64_t freeBytes=9000000;constexpr size_t RESERVE_BYTES=4194304;void refresh(){}}
struct File {std::shared_ptr<std::vector<uint8_t>> data;size_t at=0;
 operator bool()const{return bool(data);}void close(){data.reset();}void flush(){}size_t size(){return data?data->size():0;}
 size_t write(const uint8_t* p,size_t n){data->insert(data->end(),p,p+n);return n;}
 int read(uint8_t* p,size_t n){n=std::min(n,data->size()-at);memcpy(p,data->data()+at,n);at+=n;return n;}
};
struct Filesystem {
 std::map<std::string,std::shared_ptr<std::vector<uint8_t>>> files;bool failRename=false;
 bool exists(const char* p){return files.count(p);}bool mkdir(const char*){return true;}
 bool remove(const char* p){return files.erase(p);}
 bool rename(const char* a,const char* b){if(failRename&&strstr(a,".part"))return false;if(!exists(a)||exists(b))return false;files[b]=files[a];files.erase(a);return true;}
 File open(const char* p,int mode){if(!exists(p)){if(mode==FILE_READ)return {};files[p]=std::make_shared<std::vector<uint8_t>>();}return {files[p],0};}
}SD;
struct mbedtls_sha256_context{};bool hashValid=true;
void mbedtls_sha256_init(mbedtls_sha256_context*){}void mbedtls_sha256_free(mbedtls_sha256_context*){}
int mbedtls_sha256_starts(mbedtls_sha256_context*,int){return 0;}int mbedtls_sha256_update(mbedtls_sha256_context*,const uint8_t*,size_t){return 0;}
int mbedtls_sha256_finish(mbedtls_sha256_context*,uint8_t* p){for(int i=0;i<32;i++){unsigned byte;sscanf(ChakshuModel::MODEL_SHA256+i*2,"%2x",&byte);p[i]=hashValid?byte:0;}return 0;}
void vTaskDelay(int){}
${backend}
int main(){using namespace ChakshuModel;
 auto old=std::make_shared<std::vector<uint8_t>>(5,42),recording=std::make_shared<std::vector<uint8_t>>(10,7);
 SD.files[FINAL_PATH]=old;SD.files["/synap/recording.wav"]=recording;SdBackend b;
 ChakshuMedia::busy=true;assert(b.begin()==BUSY&&ChakshuMedia::busy);ChakshuMedia::busy=false;
 updating=true;assert(b.begin()==BUSY);updating=false;
 ChakshuStorage::ready=false;assert(b.begin()==NO_SD&&!busy());ChakshuStorage::ready=true;
 ChakshuStorage::freeBytes=100;assert(b.begin()==NO_SPACE&&!busy());ChakshuStorage::freeBytes=9000000;
 std::vector<uint8_t> bytes(MODEL_BYTES,99);
 assert(b.begin()==OK&&busy());b.write(bytes.data(),bytes.size());hashValid=false;assert(b.finish()==HASH_MISMATCH);b.abort();
 assert(SD.files[FINAL_PATH]==old&&!busy()&&!ChakshuMedia::busy);
 hashValid=true;assert(b.begin()==OK);b.write(bytes.data(),bytes.size());SD.failRename=true;
 assert(b.finish()==IO_ERROR&&SD.files[FINAL_PATH]==old);b.abort();SD.failRename=false;
 assert(b.begin()==OK);b.write(bytes.data(),bytes.size());assert(b.finish()==OK&&!busy());
 assert(SD.files[FINAL_PATH]->size()==MODEL_BYTES&&SD.files[BACKUP_PATH]==old);
 assert(SD.files["/synap/recording.wav"]==recording);
 // Simulate a reset after final -> backup but before staging -> final.
 SD.files.erase(FINAL_PATH);assert(restoreBackup()&&SD.files[FINAL_PATH]==old);
 assert(b.begin()==OK);b.abort();assert(SD.files[FINAL_PATH]==old);
 puts("PASS SD integrity and rollback");
}`;
  assert.match(nativeTest(body),/PASS SD integrity and rollback/);
});
