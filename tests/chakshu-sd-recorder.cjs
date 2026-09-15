'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const read=name=>fs.readFileSync('firmware/xiao-sense/'+name+'.cpp','utf8');
test('independent SD workers preserve raw PCM while camera and storage run at different rates',()=>{
 const storage=read('sd-storage');
 const wav=storage.slice(storage.indexOf('void wavHeader('),storage.lastIndexOf('\n}'));
 const fixture=fs.readFileSync('tests/chakshu-sd-recorder.cpp','utf8')
   .replace('// INSERT WAV HEADER',wav).replace('// INSERT BUFFERS',read('media-buffers'))
   .replace('// INSERT RECORDER',read('sd-recording'));
 assert.match(nativeTest(fixture,['-pthread','-Wno-unused-variable','-Wno-misleading-indentation']),/PASS independent SD capture/);
});
test('bounded media slots publish each item once and keep a held read stable across contention',()=>{
 assert.match(nativeTest(`#include <atomic>
#include <thread>
#include <cstdint>
#include <cassert>
#include <cstdio>
${read('media-buffers')}
int main(){
 ChakshuBuffers::Queue<uint32_t,2> q;uint32_t slots[2];q.slots=slots;
 *q.reserve()=17;q.publish();*q.reserve()=18;q.publish();assert(!q.reserve());
 const auto* held=q.peek();assert(*held==17);q.release();*q.reserve()=19;q.publish();
 assert(*q.peek()==18);q.release();assert(*q.peek()==19);q.release();assert(!q.peek());
 std::thread producer([&]{for(uint32_t n=0;n<50000;++n){uint32_t* slot;while(!(slot=q.reserve()))std::this_thread::yield();*slot=n;q.publish();}});
 for(uint32_t n=0;n<50000;++n){uint32_t* slot;while(!(slot=q.peek()))std::this_thread::yield();assert(*slot==n);q.release();}
 producer.join();assert(!q.peek());puts("PASS concurrent media slots");
}`,['-pthread']),/PASS concurrent media slots/);
});
