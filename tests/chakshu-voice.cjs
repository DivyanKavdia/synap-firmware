'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('Hi Chakshu gate rejects unarmed, uncertain, expired and repeated commands',()=>{
  const contract=fs.readFileSync('firmware/xiao-sense/voice-contract.cpp','utf8');
  const fixture=`#include <cstdint>
#include <cstddef>
#include <cassert>
#include <limits>
#include <cstdio>
class BLEService;
${contract}
int main(){
  using namespace ChakshuVoice;Gate gate;
  assert(gate.accept(PHOTO,1,10)==0);
  gate.accept(WAKE,.89f,10);assert(gate.accept(PHOTO,1,20)==0);
  gate.accept(WAKE,std::numeric_limits<float>::quiet_NaN(),10);assert(gate.accept(PHOTO,1,20)==0);
  gate.accept(WAKE,1.1f,10);assert(gate.accept(PHOTO,1,20)==0);
  gate.accept(WAKE,1,100);assert(gate.accept(PHOTO,1,200)==1);
  assert(gate.accept(VIDEO_START,1,2000)==0);
  gate.accept(WAKE,1,300);assert(gate.accept(PHOTO,1,400)==0);
  gate.accept(WAKE,1,500);assert(gate.accept(PHOTO,1,8501)==0);
  gate.accept(WAKE,1,10000);assert(gate.accept(AUDIO_ON,1,18000)==4);
  gate.accept(WAKE,1,20000);gate.reset();assert(gate.accept(AUDIO_OFF,1,21000)==0);
  gate.accept(WAKE,1,22000);assert(gate.accept(99,1,22010)==0);assert(gate.accept(PHOTO,1,22020)==0);
  Gate rollover;rollover.accept(WAKE,1,UINT32_MAX-100);assert(rollover.accept(VIDEO_START,1,100)==2);
  rollover.accept(WAKE,1,1400);assert(rollover.accept(VIDEO_STOP,1,1500)==3);
  rollover.accept(WAKE,1,3000);assert(rollover.accept(AUDIO_OFF,1,4000)==5);
  puts("PASS voice activation, bounds and rollover");
}`;
  assert.match(nativeTest(fixture),/PASS voice activation/);
});
