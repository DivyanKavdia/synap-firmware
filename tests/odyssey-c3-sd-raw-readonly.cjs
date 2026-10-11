'use strict';
const { test }=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const sketch=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const fn=detect.split('OdysseySdRawProbeResult odysseySdRawProbeOnce() {')[1]
  .split('const char* odysseySdMountPoint()')[0];
const op=transfer.split('case 28: {')[1].split('case 4:')[0];

test('read-only diagnosis never repairs, formats, remounts, or writes sectors',()=>{
  assert.match(fn,/sdspi_host_init_device\(&device,&handle\)/);
  assert.match(fn,/sdmmc_card_init\(&host,&card\)/);
  assert.match(fn,/sdmmc_read_sectors\(&card,sector,0,1\)/);
  assert.doesNotMatch(fn,/(?:sdmmc_write_sectors|esp_vfs_fat_sdcard_format|f_mkfs|f_mount|f_open|f_write|unlink|odysseySdMountLocked|odysseySdRecover)/);
  assert.match(fn,/odysseySdRawProbeAttempted=true/);
  assert.match(fn,/odysseySdRawProbeCached/);
  assert.match(fn,/odysseySdControllerBusyLow\(\) \|\| odysseySdBusStuckLow\(\)/);
  assert.match(fn,/odysseySdReady\(\) \|\| odysseySdNativeCard/);
});

test('raw diagnostics are explicit, one-shot and preserve SD capability state',()=>{
  assert.match(op,/odysseySdRawProbeOnce\(\)/);
  assert.match(op,/firstMountErr/);
  assert.match(op,/partitionLba/);
  assert.doesNotMatch(op,/case 19|esp_vfs_fat_sdcard_format/);
  assert.equal((transfer.match(/case 28:/g)||[]).length,1);
  assert.match(detect,/compare_exchange_strong\(expected,int32_t\(result\)\)/);
  assert(sketch.includes(detect),'checked-in compiled sketch must retain exact native SD source');
  assert(sketch.includes(transfer),'checked-in compiled sketch must retain exact BLE transfer source');
});
