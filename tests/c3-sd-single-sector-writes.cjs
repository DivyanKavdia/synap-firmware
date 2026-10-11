'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const source=rec.slice(rec.indexOf('static constexpr size_t ODYSSEY_SD_SECTOR_WRITE_BYTES=512u;'),rec.indexOf('static constexpr uint32_t ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u;'));
test('C3 4 KiB staging batch drains through exactly eight independent FatFs 512-byte writes',()=>{
 assert.match(source,/ODYSSEY_SD_SECTOR_WRITE_BYTES=512u/);
 assert.match(source,/fwrite\(data\+done,1,ODYSSEY_SD_SECTOR_WRITE_BYTES,file\)/);
 assert.match(source,/if \(written!=ODYSSEY_SD_SECTOR_WRITE_BYTES\) return done;/);
 assert.doesNotMatch(source,/retry|vTaskDelay|fseek|ftruncate|unlink|CMD25\(/);
 assert.equal((rec.match(/odysseyWritePcmBatch\(file,batch,sizeof\(batch\)\)/g)||[]).length,2);
 assert.doesNotMatch(rec,/fwrite\(batch,1,sizeof\(batch\),file\)/);
 assert.match(rec,/odysseyPersistedWriteReturned=uint32_t\(written\)/);
});
test('C3 sector writer physically produces one append-only 4096-byte WAV batch',()=>{
 const result=nativeTest(String.raw`
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cassert>
#include <iostream>
${source}
int main(){
  FILE* f=tmpfile();assert(f);
  assert(setvbuf(f,nullptr,_IONBF,0)==0);
  uint8_t pcm[4096]{};
  for(size_t i=0;i<sizeof(pcm);i++)pcm[i]=uint8_t(i);
  assert(odysseyWritePcmBatch(f,pcm,sizeof(pcm))==sizeof(pcm));
  assert(ftell(f)==4096);
  assert(fseek(f,0,SEEK_SET)==0);
  uint8_t check[4096]{};
  assert(fread(check,1,sizeof(check),f)==sizeof(check));
  assert(memcmp(check,pcm,sizeof(pcm))==0);
  assert(fclose(f)==0);
  std::cout<<"PASS checked sector append\\n";
}
`);
 assert.match(result,/PASS checked sector append/);
});
test('checkpoint cadence and no-format safety stay intact',()=>{
 assert.match(rec,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u/);
 assert.match(rec,/odysseyCheckpointWav\(file,checkpointErrno\)/);
 assert.doesNotMatch(rec,/ftruncate\(|fseek\(file,0/);
 const backend=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
 assert.match(backend,/mount\.format_if_mount_failed=false/);
 assert.match(backend,/host\.do_transaction=\[\]/);
});
