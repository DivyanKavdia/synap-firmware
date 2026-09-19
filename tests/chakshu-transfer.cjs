'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
test('SD transfer closes every file before releasing the gate and survives remount between chunks',()=>{
 const helpers=source.slice(source.indexOf('void clearSelection()'),source.indexOf('bool validPath('));
 const fixture=fs.readFileSync('tests/chakshu-transfer.cpp','utf8');
 assert.match(nativeTest(fixture.replace('// INSERT FILE HELPERS',helpers)),/PASS SD remount/);
});
test('old transfer results cannot masquerade as the new connection transaction',()=>{
 const reply=source.slice(source.indexOf('void reply('),source.indexOf('void saveOffline('));
 const fixture=`#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <cstdio>
std::atomic<uint32_t> connectionGeneration{3};
struct Request {uint32_t connection,id;uint8_t operation=0;bool local=false;};
namespace ChakshuVoice {int completions=0;uint8_t lastOperation=0,lastError=0;void mediaCompleted(uint8_t op,uint8_t error){++completions;lastOperation=op;lastError=error;}}
int mux=0;void portENTER_CRITICAL(int*){}void portEXIT_CRITICAL(int*){}
uint8_t response[496]{};size_t responseSize=16;uint32_t responseConnection=0;
void put32le(uint8_t* p,uint32_t n){for(int i=0;i<4;++i)p[i]=n>>(8*i);}
${reply}
int main(){
 Request old{3,1};uint8_t received[496],body[3]={8,9,10};size_t size=0;
 replyFor(old,0,3,0,body,3);readResponse(received,size);assert(size==19&&received[4]==1&&received[16]==8);
 ++connectionGeneration;
 replyFor(old,0,3,0,body,3);readResponse(received,size);assert(size==16&&received[2]==0&&received[4]==0);
 Request current{4,1};replyFor(current,0,3,0,body,3);readResponse(received,size);
 assert(size==19&&received[2]==1&&received[4]==1&&received[18]==10);
 replyFor(old,7);readResponse(received,size);
 assert(size==19&&received[3]==0&&received[18]==10);
 Request photo{4,9,11,true};replyFor(photo,0);
 assert(ChakshuVoice::completions==1&&ChakshuVoice::lastOperation==11&&ChakshuVoice::lastError==0);
 Request video{4,10,5,true};replyFor(video,0);assert(ChakshuVoice::completions==1);
 replyFor(video,3);assert(ChakshuVoice::completions==2&&ChakshuVoice::lastError==3);
 readResponse(received,size);assert(received[4]==1); // Local jobs cannot overwrite app replies.
 puts("PASS connection-owned transfer response");
}`;
 assert.match(nativeTest(fixture),/PASS connection-owned transfer response/);
});
