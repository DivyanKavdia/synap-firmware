'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const read=p=>fs.readFileSync(p,'utf8');
const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');
const detect=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const power=read('firmware/shared/power.cpp');
const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');

test('C3 persisted stage 70 diagnostics migrate from legacy NVS and survive software reset',()=>{
  const start=recorder.indexOf('static std::atomic<uint8_t> odysseyPersistedRecordStage');
  const end=recorder.indexOf('static void odysseyWavHeader(uint8_t* h',start);
  assert(start>=0&&end>start,'extract active production diagnostic code');
  const source=recorder.slice(start,end);
  const output=nativeTest(String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <vector>
#include <cstdio>
#include <iostream>
static std::vector<uint8_t> saved, extra;
struct Preferences {
  bool begin(const char*,bool){return true;}
  std::vector<uint8_t>& bucket(const char* k){
    return std::strcmp(k,"write")==0?extra:saved;
  }
  size_t getBytesLength(const char* k){return bucket(k).size();}
  size_t getBytes(const char* k,void* out,size_t n){
    auto& value=bucket(k);if(n>value.size())return 0;
    std::memcpy(out,value.data(),n);return n;
  }
  size_t putBytes(const char* k,const void* p,size_t n){
    const auto* b=static_cast<const uint8_t*>(p);bucket(k).assign(b,b+n);return n;
  }
  void end(){}
};
${source}
static void resetLoaded(){
  odysseyPersistedRecordLoaded=false;
  odysseyPersistedRecordStage=0;odysseyPersistedRecordBytes=0;
  odysseyPersistedWriteErrno=0;odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=0;odysseyPersistedWriteFerror=0;
}
int main(){
  uint32_t old[3]={1,70,2040u*1024u};
  saved.assign(reinterpret_cast<uint8_t*>(old),reinterpret_cast<uint8_t*>(old)+sizeof(old));
  resetLoaded();
  assert(odysseyLastRecordFailureStage()==70);
  assert(odysseyLastRecordFailureBytes()==2040u*1024u);
  assert(odysseyLastWriteExpected()==0);

  odysseyPersistedWriteErrno=0;
  odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=4096;
  odysseyPersistedWriteFerror=1;
  odysseyPersistRecordFailure(70,2040u*1024u);
  // Legacy firmware still reads the original three-word "last" record.
  assert(saved.size()==3u*sizeof(uint32_t));
  assert(extra.size()==6u*sizeof(uint32_t));
  uint32_t retained[3]{};std::memcpy(retained,saved.data(),sizeof(retained));
  assert(retained[0]==1&&retained[1]==70&&retained[2]==2040u*1024u);
  resetLoaded();
  assert(odysseyLastRecordFailureStage()==70);
  assert(odysseyLastRecordFailureBytes()==2040u*1024u);
  assert(odysseyLastWriteErrno()==0);
  assert(odysseyLastWriteReturned()==0);
  assert(odysseyLastWriteExpected()==4096);
  assert(odysseyLastWriteFerror()==1);

  odysseyPersistRecordFailure(0,0);
  resetLoaded();
  assert(odysseyLastRecordFailureStage()==0);
  assert(odysseyLastWriteExpected()==0);
  std::cout<<"PASS C3 SD write diagnostics\n";
}
`);
  assert.match(output,/PASS C3 SD write diagnostics/);
});

test('failed 4 KiB writes record errno, returned byte count and ferror before closing file',()=>{
  assert.equal((recorder.match(/odysseyPersistedWriteExpected=sizeof\(batch\);/g)||[]).length,2);
  assert.equal((recorder.match(/odysseyPersistedWriteFerror=ferror\(file\)\?1u:0u;/g)||[]).length,2);
  assert.match(recorder,/const int writeError=errno;\s+odysseyPersistedWriteErrno=/);
  assert.match(recorder,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.match(recorder,/odysseyPersistRecordFailure\(0,0\)/);
  assert.match(recorder,/stage\?1u:0u/);
  assert.match(recorder,/prefs\.putBytes\("write",detail,sizeof\(detail\)\)/);
  assert.match(recorder,/prefs\.putBytes\("last",record,sizeof\(record\)\)/);
  assert.match(recorder,/legacy\?record\[0\]==1u:record\[0\]==2u/);
  assert.doesNotMatch(recorder,/fseek\(file,0|ftruncate\(/);
});

test('stuck-low MISO only halts AUTOMATIC sleep recovery, never explicit card recovery',()=>{
  const testBlock=detect.split('bool odysseySdBusStuckLow() {')[1].split('}')[0];
  assert.match(testBlock,/odysseySdBootState\.load\(\)!=1/);
  assert.match(testBlock,/odysseySdBitBangCsHigh\.load\(\)==0/);
  assert.match(testBlock,/odysseySdRawZero\.load\(\)>=900u/);
  const sleep=power.split('void enterDeepSleep(const char* reason) {')[1].split('void powerTick() {')[0];
  assert(sleep.indexOf('odysseySdBusStuckLow()')<sleep.indexOf('odysseyRecoverSdCard("sleep")'));
  assert.match(sleep,/if \(odysseySdBusStuckLow\(\)\) return;/);
  assert.match(detect,/odysseyRecoverSdCard\(const char\* reason\)/);
  assert.match(transfer,/case 14:[\s\S]*?odysseyRecoverSdCard\("op14"\)/);
  assert.match(recorder,/odysseyRecoverSdCard\("touch"\)/);
  assert.match(power,/if \(!odysseyPrepareSdForPowerTransition\(1000u\)\)/);
});

test('catalogue failure report exposes short-write details with older diagnostics',()=>{
  for (const key of ['wrE','wrN','wrX','wrF','bbHigh','raw0','bbCmd0','bbR7'])
    assert(transfer.includes(String.raw`\"`+key+String.raw`\"`),key+' present');
  for (const name of ['odysseyLastWriteErrno','odysseyLastWriteReturned',
    'odysseyLastWriteExpected','odysseyLastWriteFerror'])
    assert(transfer.includes(name+'()'),name);
  assert.match(transfer,/char detail\[480\]/);
});
