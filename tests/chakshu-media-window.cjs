'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
test('camera notification windows bound credit, reserve audio capacity and cancel stale or queued work',()=>{
 const code=source.slice(source.indexOf('bool chakshuAudioHasBacklog();'),source.indexOf('void worker(void*)'));
 assert.match(nativeTest(`#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=65535;
uint32_t now=0;uint32_t millis(){return now;}
uint32_t pdMS_TO_TICKS(uint32_t ms){return ms;}void vTaskDelay(uint32_t ms){now+=ms;}
struct Request {uint32_t connection=1,id=7,offset=0,windowEpoch=0;};
std::atomic<uint16_t> chakshuConnectionHandle{7};
std::atomic<uint32_t> connectionGeneration{1};
std::atomic<bool> deviceConnected{true},streamingEnabled{false};
struct BLECharacteristic {uint16_t getHandle(){return 42;}} characteristic;
struct Server {uint16_t mtu=517;uint16_t getPeerMTU(uint16_t){return mtu;}} server;
auto* bleServer=&server;
struct os_mbuf {std::vector<uint8_t> bytes;};
int freeBlocks=24,live=0,reject=0;bool allocationFail=false,backlog=false;
std::vector<std::vector<uint8_t>> packets;
int os_msys_num_free(){return freeBlocks;}
os_mbuf* ble_hs_mbuf_from_flat(const uint8_t* data,size_t n){if(allocationFail)return nullptr;++live;freeBlocks-=2;return new os_mbuf{{data,data+n}};}
void os_mbuf_free_chain(os_mbuf* p){delete p;--live;freeBlocks+=2;}
int ble_gattc_notify_custom(uint16_t connection,uint16_t handle,os_mbuf* p){assert(connection==7&&handle==42);if(!reject)packets.push_back(p->bytes);os_mbuf_free_chain(p);return reject;}
bool chakshuAudioHasBacklog(){return backlog;}
void put32le(uint8_t* p,uint32_t n){memcpy(p,&n,4);}
namespace ChakshuMedia {constexpr int BAD_COMMAND=2;}
void replyFor(const Request&,int){assert(false);}
uint8_t readSelection(uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size){
 total=10000;if(offset>=total)return 7;size=std::min(size_t(480),size_t(total-offset));
 for(size_t i=0;i<size;i++)bytes[i]=uint8_t(offset+i);return 0;
}
${code}
uint32_t word(const std::vector<uint8_t>& p,int at){uint32_t n;memcpy(&n,p.data()+at,4);return n;}
int main(){
 Request r;streamCharacteristic=&characteristic;subscribedConnection=7;
 streamWindow(r);assert(packets.size()==9 && packets.back()[2]==2 && word(packets.back(),12)==3840);
 for(unsigned n=0;n<8;n++){assert(packets[n].size()==496&&word(packets[n],12)==n*480);for(unsigned i=16;i<496;i++)assert(packets[n][i]==uint8_t(n*480+i-16));}
 packets.clear();server.mtu=23;streamWindow(r);assert(packets.size()==9&&packets[0].size()==20&&word(packets.back(),12)==32);
 packets.clear();server.mtu=517;streamingEnabled=true;backlog=true;streamWindow(r);
 assert(packets.size()==1 && packets[0][2]==2 && word(packets[0],8)==0); // audio has priority
 packets.clear();++cancelWindow;streamWindow(r);assert(packets.empty()); // Stop before queued work starts
 r.windowEpoch=cancelWindow;backlog=false;freeBlocks=8;streamWindow(r);assert(packets.empty()&&!live&&freeBlocks==8);
 freeBlocks=9;streamWindow(r);assert(packets.empty()&&!live&&freeBlocks==9); // post-allocation reserve check
 freeBlocks=24;allocationFail=true;streamWindow(r);assert(packets.empty()&&!live);
 allocationFail=false;reject=6;streamWindow(r);assert(packets.empty()&&!live);
 reject=0;subscribedConnection=65535;streamWindow(r);assert(packets.empty());
 subscribedConnection=7;++connectionGeneration;streamWindow(r);assert(packets.empty());
 puts("PASS paced camera credit, audio priority, native ownership and cancellation");
}`,['-Wno-misleading-indentation']),/PASS paced camera credit/);
});
