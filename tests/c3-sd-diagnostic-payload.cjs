'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('full-width C3 mount and root-failure diagnostics remain valid JSON within BLE payload',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-transfer.cpp','utf8');
 const start=source.indexOf('      char detail[480];');
 const end=source.indexOf('      reply(request,error,total,request.offset',start);
 assert(start>=0&&end>start);
 const unsigned=['odysseySdDetectionState','odysseySdProbeState','odysseySdAttemptCount',
 'odysseySdBeginAttemptCount','odysseySdReleaseAttemptCount','odysseySdLastMountReasonCode',
 'odysseySdRecordFailureStage','odysseySdRecordLastBytes'];
 const signed=['odysseySdLastError','odysseySdLastIoError','odysseySdLastReleaseErrorCode'];
 const program=`
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <algorithm>
${unsigned.map(n=>`uint32_t ${n}(){return UINT32_MAX;}`).join('\n')}
${signed.map(n=>`int32_t ${n}(){return INT32_MIN;}`).join('\n')}
uint64_t odysseySdLastFreeByteCount(){return UINT64_MAX;}
uint64_t odysseySdLastGoodFreeByteCount(){return UINT64_MAX;}
std::atomic<int> odysseySdInitDetail[5];
uint32_t odysseyStoredStage=UINT32_MAX,odysseyStoredBytes=UINT32_MAX,odysseyStoredBuild=UINT32_MAX;
uint32_t odysseyRootStage=UINT32_MAX,odysseyRootBytes=UINT32_MAX,odysseyRootBuild=UINT32_MAX;
int32_t odysseyStoredErrno=INT32_MIN,odysseyRootErrno=INT32_MIN;
int catalogueErrno=INT32_MIN;
int main(){
 for(auto& field:odysseySdInitDetail)field=INT32_MIN;
 ${source.slice(start,end)}
 assert(n>0&&size_t(n)<sizeof(detail)&&strlen(detail)==size_t(n));
 puts(detail);
}`;
 // Deliberately overflow the verbose format to execute its checked compact fallback.
 const text=nativeTest(program,['-Wno-format-truncation']),decoded=JSON.parse(text);
 assert(Buffer.byteLength(text.trim())<=480);
 assert.equal(decoded.rootRecordErrno,-2147483648);
 assert.equal(decoded.lastRecordBytes,4294967295);
 assert.deepEqual(decoded.sdInit,Array(5).fill(-2147483648));
});
