'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {nativeTest}=require('./support/native.cjs');
const read=p=>fs.readFileSync(require('node:path').join(__dirname,'..',p),'utf8');
const tx=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
test('compressed SD sender preserves source offsets, partial tail, retry identity and fault gates',()=>{
 const codec=read('firmware/shared/audio-transport.cpp').split('// Only the transmitter')[0];
 const sender=tx.slice(tx.indexOf('static uint32_t sdSyncCrc32'),tx.indexOf('\n#endif\n\nstatic void streamSdWindow'));
 const result=nativeTest(`
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cerrno>
constexpr int SAMPLES_PER_FRAME=800, ADPCM_HEADER_BYTES=4, AUDIO_CODEC_IMA_ADPCM=1;
${codec}
struct Request {uint32_t connection=1,id=7,offset=0,sessionToken=9;bool compact=true;};
std::atomic<uint32_t> connectionGeneration{1};std::atomic<bool> deviceConnected{true},streamingEnabled{false},odysseyRecording{false};
std::atomic<uint16_t> attValueCapacity{514};bool sleepPending=false,quarantined=false,failRead=false;
uint32_t selectedSessionToken=9,selectedSessionConnection=1,selectedFileSize=44+1600*8+6;
char selectedPath[]="/synap/test.WAV";
int sdBleReadWindowRequests=0,sdBleReadFailureOffset=0,sdBleReadLastRequestedOffset=0,sdBleReadFailureStep=0,sdBleReadFailureErrno=5;
enum {OK=0,BAD_COMMAND=2,IO_ERROR=7,FILE_UNAVAILABLE=11};
bool otaBusy(){return false;}void vTaskDelay(int){}int pdMS_TO_TICKS(int n){return n;}
void put32le(uint8_t*p,uint32_t n){for(int i=0;i<4;i++)p[i]=uint8_t(n>>(8*i));}
uint32_t u32(const std::vector<uint8_t>&p,int i){return uint32_t(p[i])|(uint32_t(p[i+1])<<8)|(uint32_t(p[i+2])<<16)|(uint32_t(p[i+3])<<24);}
void noteSdMediaFault(int,int,int){}void odysseySdMarkVfsFailure(){quarantined=true;}void invalidateBleReadCache(){}
std::vector<std::vector<uint8_t>> packets;
struct Stream {std::vector<uint8_t> p;void setValue(uint8_t*b,size_t n){p.assign(b,b+n);}void notify(){packets.push_back(p);}} stream;
Stream*streamCharacteristic=&stream;
void notifySdWindow(const Request&r,uint8_t kind,uint8_t error,uint32_t total,uint32_t offset,const uint8_t*p=nullptr,size_t n=0){
 std::vector<uint8_t>b(16+n);b[0]=0xcc;b[1]=1;b[2]=kind;b[3]=error;put32le(b.data()+4,r.id);put32le(b.data()+8,total);put32le(b.data()+12,offset);if(n)memcpy(b.data()+16,p,n);packets.push_back(b);
}
uint8_t readSelected(const char*,uint32_t offset,uint32_t&total,uint8_t*p,size_t&n){
 if(failRead)return IO_ERROR;
 total=selectedFileSize;
 n=std::min({size_t(480),size_t(total-offset),size_t(4096-offset%4096)});
 for(size_t i=0;i<n;i++)p[i]=uint8_t((offset+i)*7);
 return OK;
}
${sender}
int main(){
 assert(sdSyncCrc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
 Request r;streamCompressedSdWindow(r);assert(packets.size()==7);assert(packets[0].size()==60);
 uint32_t next=44;
 for(size_t i=1;i<6;i++){auto&p=packets[i];assert(p.size()==426&&p[2]==3&&u32(p,12)==next);assert(u32(p,422)==sdSyncCrc32(p.data(),422));next+=1600;}
 assert(u32(packets.back(),12)==next);auto original=packets[2];
 packets.clear();r.offset=1644;streamCompressedSdWindow(r);assert(packets[0]==original);
 packets.clear();r.offset=44+1600*8;streamCompressedSdWindow(r);assert(packets.size()==2&&packets[0][16]==3&&u32(packets[1],12)==selectedFileSize);
 packets.clear();r.sessionToken=8;streamCompressedSdWindow(r);assert(packets.size()==1&&packets[0][3]==FILE_UNAVAILABLE);
 packets.clear();r.sessionToken=9;r.offset=45;streamCompressedSdWindow(r);assert(packets[0][3]==BAD_COMMAND);
 packets.clear();r.offset=44;attValueCapacity=244;streamCompressedSdWindow(r);assert(packets.size()==1&&u32(packets[0],8)==0);
 packets.clear();attValueCapacity=514;streamingEnabled=true;streamCompressedSdWindow(r);assert(packets.empty());streamingEnabled=false;
 failRead=true;streamCompressedSdWindow(r);assert(quarantined&&packets.size()==1&&packets[0][3]==IO_ERROR);
 puts("PASS");
}`);
 assert.match(result,/PASS/);
});
test('isolated publish selectors choose exactly the intended artifacts and keep the global index',()=>{
 const pub=read('tools/publish.cjs'),check=read('tools/check-feed.cjs');
 const artifacts=Object.values(require('../tools/targets.cjs').TARGETS).map(config=>({config}));
 const expression=pub.match(/const selected=(.*);/)[1];
 for(const [onlyTarget,ids] of [['c3-sd-only',['esp32c3-supermini-4m-sd','esp32c3-supermini-4m-sd-ota']],['xiao-esp32s3-sense-8m',['xiao-esp32s3-sense-8m']],['',artifacts.map(x=>x.config.id)]]){
  assert.deepEqual(Array.from(vm.runInNewContext(expression,{artifacts,onlyTarget}),x=>x.config.id),ids);
  const expr=check.match(/const checked=(.*);/)[1];
  assert.deepEqual(Array.from(vm.runInNewContext(expr,{local:artifacts,onlyTarget}),x=>x.config.id),ids);
 }
 assert.match(pub,/if\(!onlyTarget\)treeEntries\.push/);
 const {renderProfile}=require('../tools/device-profile.cjs');
 for(const {config} of artifacts)assert.equal(renderProfile(config).includes('#define SYNAP_C3_SD_COMPRESSED_SYNC 1'),config.id.endsWith('-sd')||config.id.endsWith('-sd-ota'));
});
