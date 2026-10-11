'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const sketch=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const probe=detect.split('void odysseySdRawProbeOnce() {')[1]
  .split('const char* odysseySdMountPoint()')[0];
const reply=transfer.split('case 28: {')[1].split('case 4:')[0];
test('physical sector 0 and at most one bounds-checked FAT VBR read; never repair',()=>{
  assert.match(probe,/sdmmc_read_sectors\(&card,bootBlock,0,1\)/);
  assert.match(probe,/sdmmc_read_sectors\(\s*&card,bootBlock,result\.partitionLba,1\)/);
  assert.equal((probe.match(/sdmmc_read_sectors\(/g)||[]).length,2);
  assert.match(probe,/card\.csd\.sector_size!=512u/);
  assert.match(probe,/uint64_t\(lba\)\+uint64_t\(count\)<=uint64_t\(result\.sectors\)/);
  assert.match(probe,/if \(result\.partitionBounds\) \{/);
  assert.match(probe,/for \(unsigned entry=0;entry<4;\+\+entry\)/);
  assert.match(probe,/result\.partitionType=type/);
  assert.match(probe,/result\.vbrRead=2;result\.vbrError=vbrErr/);
  assert.doesNotMatch(probe,/sdmmc_write_sectors|f_mkfs|esp_vfs_fat_sdcard_format|\bfwrite\b|\bunlink\b|odysseyRecoverSdCard/);
});
test('FAT plausibility checks geometry but cannot claim file-table validation',()=>{
  for(const field of ['bpbPlausible','fatVariant','partitionSectors','partitionBounds','bpbTotalSectors','vbrSignature','vbrError'])
    assert.match(probe,new RegExp('result\\.'+field));
  assert.match(probe,/bytesPerSector==512/);
  assert.match(probe,/fats==1 \|\| fats==2/);
  assert.match(probe,/reserved>=1/);
  assert.match(probe,/overhead<total/);
  assert.match(probe,/uint8_t variant=clusters<4085u\?12:/);
  assert.match(probe,/clusters<65525u\?16:32/);
  assert.match(probe,/rootEntries==0/);
  assert(reply.includes('version') && reply.includes('status'),'response includes version and status');
  assert.match(reply,/bpbPlausible/);
  assert.match(reply,/firstMountErr/);
  assert.match(reply,/char detail\[480\]/);
  assert(sketch.includes(detect));assert(sketch.includes(transfer));
});
