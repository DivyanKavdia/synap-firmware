'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const sketch=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const chunk=tx.split('static uint8_t readWavChunk(')[1].split('static uint8_t readSelected(')[0];
const read=tx.split('static uint8_t readSelected(')[1].split('static uint8_t catalogue(')[0];
const win=tx.split('static void streamSdWindow(')[1].split('static void worker(')[0];
const op4=tx.split('case 4:')[1].split('case 7:')[0];
test('C3 BLE WAV sync caches 4KiB CLOSED read chunks rather than reopen per 480-byte packet',()=>{
  assert.match(tx,/ODYSSEY_SD_BLE_READ_CACHE_BYTES=4096u/);
  assert.match(tx,/alignas\(4\) uint8_t bytes\[ODYSSEY_SD_BLE_READ_CACHE_BYTES\]/);
  assert.match(chunk,/cache\.valid && cache\.connection==generation/);
  assert.match(chunk,/!strcmp\(cache\.path,logical\)/);
  assert.match(chunk,/uint64_t\(offset\)<uint64_t\(cache\.start\)\+cache\.used/);
  assert.match(chunk,/fopen\(full,"rb"\)/);
  assert.match(chunk,/const size_t step=std.min/);
  assert.match(chunk,/fread\\(sdBleReadCache\\.bytes\\+got,1,step,file\\)/);
  assert.doesNotMatch(chunk,/fread\\(sdBleReadCache\\.bytes,1,want,file\\)/);
  assert.match(chunk,/fclose\(file\)/);
  assert.match(chunk,/sdBleReadCache\.valid=true/);
  assert(chunk.indexOf('fclose(file)')<chunk.indexOf('sdBleReadCache.valid=true'));
  assert.doesNotMatch(read,/fopen\(|fseek\(|fread\(|fclose\(/);
  assert.doesNotMatch(chunk,/fwrite|f_mkfs|sdmmc_write_sectors|esp_vfs_fat_sdcard_format|unlink\(/);
});
test('C3 cache is connection-bound, storage-gated, self describing and invalidated on mutations',()=>{
  assert.match(chunk,/if \(!guard \|\| !storageReady\(\)\)/);
  assert.match(chunk,/const uint32_t generation=connectionGeneration\.load\(\)/);
  assert.match(chunk,/snprintf\(sdBleReadCache\.path/);
  assert.match(chunk,/patchVirtualWavHeader\(/);
  assert.match(read,/readWavChunk\(requestedPath,offset,total,bytes,size\)/);
  assert.match(read,/readWavChunk\(selectedPath,offset,total,bytes,size\)/);
  for (const op of ['14','17','18','19']) assert.match(tx,new RegExp('case '+op+':(?: \\{)?\\n\\s*invalidateBleReadCache\\(\\)'));
  assert(sketch.includes(tx),'compiled production sketch must exactly mirror shared transfer');
});
test('C3 media read failures retain live fault operation and step separately from historical CMD25',()=>{
  for(const [section,op] of [[op4,4],[win,12]]) {
    assert.match(section,new RegExp('noteSdMediaFault\\('+op+',sdBleReadFailureStep,sdBleReadFailureErrno\\)'));
    assert.match(section,/odysseySdMarkVfsFailure\(\)/);
    assert.match(section,/invalidateBleReadCache\(\)/);
  }
  for(const x of ['sdBleReadFailureStep=1','sdBleReadFailureStep=2','sdBleReadFailureStep=!seekOk?3:got!=want?4:5'])
    assert(chunk.includes(x));
  assert.match(tx,/case 27: \{/);
  assert.match(tx,/sdLastMediaFaultOperation\.load\(\)/);
});
