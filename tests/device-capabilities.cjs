'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const {TARGETS,FLAGS,capabilityMask}=require('../tools/targets.cjs');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {profileBlock}=require('../tools/device-profile.cjs');
const {nativeTest}=require('./support/native.cjs');
for(const target of Object.values(TARGETS)) {
  test(`${target.name} advertises only its supported and initialized hardware`,()=>{
    const source=materialize(assemble(),target.id);
    const start=source.indexOf('void encodeModuleCapabilities(uint8_t* p) {');
    const encode=source.slice(start,source.indexOf('class ModuleCapabilitiesCallbacks',start));
    const body=`#include <cstdint>
#include <cstring>
#include <atomic>
#include <cassert>
#include <cstdio>
${profileBlock(source)}
#define USE_REAL_I2S_MIC 1
constexpr uint16_t SAMPLE_RATE=16000;
std::atomic<bool> microphoneValidated{false};
bool batteryAvailable=false;
struct Esp { unsigned getFlashChipSize(){return ${target.flashBytes};} unsigned getPsramSize(){return ${target.psramBytes};} } ESP;
namespace ChakshuMedia {struct Snapshot {uint16_t ready=0,sensor=0;};Snapshot current;void copy(Snapshot& s){s=current;}}
namespace ChakshuTransfer {bool requests=true;}
${encode}
unsigned word(const uint8_t* p){return p[0]|unsigned(p[1])<<8;}
int main(){
 uint8_t p[20];
 for(unsigned hardware=0;hardware<8;++hardware){
  microphoneValidated=hardware&1;batteryAvailable=hardware&4;
  ChakshuMedia::current.ready=hardware;ChakshuMedia::current.sensor=(hardware&2)?0x3660:0;
  encodeModuleCapabilities(p);
  assert(p[0]==0xC7 && p[1]==1 && p[2]==${target.moduleId} && p[3]==1);
  assert(word(p+4)==${capabilityMask(target)});
  assert((word(p+6)&~word(p+4))==0);
  assert(word(p+10)==16000 && p[12]==${target.flashBytes/1048576} && p[13]==${target.psramBytes/1048576});
  assert(bool(word(p+6)&${FLAGS.audio})==bool(hardware&1));
#if SYNAP_CHAKSHU
  assert(bool(word(p+6)&SYNAP_CAP_PHOTO)==bool(hardware&2));
  assert(bool(word(p+6)&SYNAP_CAP_SDAUDIO)==((hardware&5)==5));
  assert(!(word(p+4)&(SYNAP_CAP_TOUCH|SYNAP_CAP_BATTERY|SYNAP_CAP_STANDBY)));
  assert(p[14]==1 && p[15]==0 && p[16]==31);
#else
  assert(!(word(p+4)&(SYNAP_CAP_CAMERA|SYNAP_CAP_SD|SYNAP_CAP_PHOTO|SYNAP_CAP_VIDEO)));
  assert(p[14]==0 && p[15]==0 && p[16]==0);
#endif
 }
 puts("PASS device capabilities");
}`;
    assert.match(nativeTest(body),/PASS device capabilities/);
  });
}
