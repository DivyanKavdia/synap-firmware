'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {nativeTest}=require('./support/native.cjs');
const read=p=>fs.readFileSync(require('node:path').join(__dirname,'..',p),'utf8');
const tx=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
test('lossless compact windows remove exactly one redundant wait and preserve all bytes',()=>{
 const codec=read('firmware/shared/audio-transport.cpp').split('// Only the transmitter')[0];
 const sender=tx.slice(tx.indexOf('static void streamSdWindow('),tx.indexOf('static void worker(void*)'));
 const result=nativeTest(`
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cassert>
#include <cstdio>
#include <cerrno>
#define SYNAP_C3_SD_COMPRESSED_SYNC 1
using TickType_t=int;
int sdBleReadMtuFallbacks=0;namespace OdysseyWifi {bool busy(){return false;}}
constexpr int SAMPLES_PER_FRAME=800, ADPCM_HEADER_BYTES=4, AUDIO_CODEC_IMA_ADPCM=1;
${codec}
struct Request {uint32_t connection=1,id=7,offset=0,sessionToken=9;bool compact=true;uint8_t operation=31,credits=8,paceMs=10;char path[64]{};};
std::atomic<uint32_t> connectionGeneration{1};std::atomic<bool> deviceConnected{true},streamingEnabled{false},odysseyRecording{false};
std::atomic<uint16_t> attValueCapacity{514};bool sleepPending=false,quarantined=false,failRead=false;
uint32_t selectedSessionToken=9,selectedSessionConnection=1,selectedFileSize=44+1600*8+6;
char selectedPath[]="/synap/test.WAV";
int sdBleReadWindowRequests=0,sdBleReadFailureOffset=0,sdBleReadLastRequestedOffset=0,sdBleReadFailureStep=0,sdBleReadFailureErrno=5;
enum {OK=0,BAD_COMMAND=2,IO_ERROR=7,FILE_UNAVAILABLE=11};
bool otaBusy(){return false;}int waitMs=0;void vTaskDelay(int n){waitMs+=n;}int pdMS_TO_TICKS(int n){return n;}
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
 Request r;r.operation=31;r.credits=8;r.paceMs=10;
 streamSdWindow(r);assert(packets.size()==9);assert(waitMs==80);assert(u32(packets.back(),12)==3840);
 for(int i=0;i<8;i++){assert(packets[i].size()==496);for(int j=0;j<480;j++)assert(packets[i][16+j]==uint8_t((i*480+j)*7));}
 packets.clear();waitMs=0;r.operation=12;streamSdWindow(r);assert(waitMs==140);assert(packets.size()==7);
 puts("PASS");
}`);
 assert.match(result,/PASS/);
});
