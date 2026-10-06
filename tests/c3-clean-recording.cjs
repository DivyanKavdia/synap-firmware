'use strict';
const {test}=require('node:test'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('clean recorder saves exact PCM and preserves partials on write/sync/close failure',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const header=source.slice(source.indexOf('static void odysseyCleanWavHeader'),source.indexOf('static uint8_t odysseyCleanRawByte'));
 const recorder=source.slice(source.indexOf('static bool odysseyCleanWriteAll'),source.indexOf('namespace OdysseyTransfer'));
 const body=fs.readFileSync('tests/c3-clean-recording.cpp','utf8').replace('// INSERT PRODUCTION',(header+recorder).replaceAll('/odyssey-sd/synap/',''));
 nativeTest(body,['-DUSE_REAL_I2S_MIC=1']);
});
test('pre-mount transfer stop is bounded and never sends a write-stop token while busy',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const stop=source.slice(source.indexOf('static void odysseyCleanStopOldTransfer'),source.indexOf('// Minimal pre-mount'));
 nativeTest(`
#include <atomic>
#include <cstdint>
#include <cassert>
#include <vector>
#include <algorithm>
constexpr int ODYSSEY_SD_CS=0,LOW=0,HIGH=1;
std::atomic<bool> odysseyStopRequested{false};
uint32_t clockMs=0;bool busy=false;
std::vector<uint8_t> sent;
uint32_t millis(){return clockMs;}
void digitalWrite(int,int){}
uint8_t odysseyCleanRawByte(uint8_t b){++clockMs;sent.push_back(b);return busy?0:255;}
void odysseyCleanIdleClocks(uint8_t bytes){while(bytes--)(void)odysseyCleanRawByte(255);}
${stop}
int main(){
 odysseyCleanStopOldTransfer();assert(clockMs<800);assert(sent[0]==0x4c&&sent[5]==0x61);
 assert(std::find(sent.begin(),sent.end(),0xfd)!=sent.end());
 busy=true;clockMs=0;sent.clear();odysseyCleanStopOldTransfer();assert(clockMs<800);
 assert(std::find(sent.begin(),sent.end(),0xfd)==sent.end());
}
`);
});
