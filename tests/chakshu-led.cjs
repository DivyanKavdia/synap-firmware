'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('SD recording LED blinks active-low and clears on completion or error',()=>{
  const source=fs.readFileSync('firmware/xiao-sense/status-led.cpp','utf8');
  assert.match(nativeTest(`#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
constexpr int LOW=0,HIGH=1;
uint32_t now=0;int level=HIGH;
uint32_t millis(){return now;}
void digitalWrite(int pin,int value){assert(pin==21);level=value;}
namespace ChakshuTransfer {std::atomic<bool> offline{false};}
${source}
int main(){
 for(now=0;now<2000;++now){updateStatusLed(false);assert(level==HIGH);}
 ChakshuTransfer::offline=true;
 for(now=0;now<3000;++now){updateStatusLed(false);assert(level==(now%1000<250?LOW:HIGH));}
 now=100;updateStatusLed(true);assert(level==LOW);
 ChakshuTransfer::offline=false;updateStatusLed(false);assert(level==HIGH);
 now=0xffffffff;ChakshuTransfer::offline=true;updateStatusLed(false);
 now=0;updateStatusLed(false);assert(level==LOW);
 ChakshuTransfer::offline=false;updateStatusLed(true);assert(level==HIGH);
 puts("PASS SD recording LED");
}`),/PASS SD recording LED/);
});
