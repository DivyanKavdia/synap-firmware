'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const base=path.resolve(__dirname,'..');
const load=p=>fs.readFileSync(path.join(base,p),'utf8');
const transfer=load('firmware/shared/odyssey-sd-1631-transfer.cpp');
const caps=load('firmware/shared/module-capabilities.cpp');
const ino=load('synap_esp32s3/synap_esp32s3.ino');
test('C3 adds an optional media-v1 notification window without replacing legacy reads',()=>{
  assert.match(caps,/p\[16\]\|=1/);
  assert.match(transfer,/PROPERTY_NOTIFY/);
  assert.match(transfer,/4fa1235a-0000-1000-8000-00805f9b34fb/);
  assert.match(transfer,/case 12:\s*streamSdWindow\(request\);[\s\S]*?sdBleTransferInFlight=false;[\s\S]*?continue;/);
  assert.match(transfer,/case 4:\s*error=readSelected\(request.path,request.offset,total,bytes,size\)/);
  assert.match(transfer,/case 16: error=OK; break;/);
  assert.match(transfer,/OdysseySdGuard guard/);
});
test('C3 notifications are paced, bounded, explicit-path, and MTU safe',()=>{
  const fn=transfer.split('static void streamSdWindow(const Request& request) {')[1]
    .split('static void worker(void*) {')[0];
  assert.match(fn,/capacity<496u/);
  assert.match(fn,/request\.path\[0\]/);
  assert.match(fn,/n<6/);
  assert.match(fn,/readSelected\(request\.path,offset,total,chunk,length\)/);
  assert.match(fn,/notifySdWindow\(request,1,OK,fullSize,offset,chunk,length\)/);
  assert.match(fn,/vTaskDelay\(pdMS_TO_TICKS\(20\)\)/);
  assert.match(fn,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
  assert.match(fn,/odysseySdMarkVfsFailure\(\)/);
  assert.match(fn,/streamingEnabled\.load\(\) \|\| odysseyRecording\.load\(\)/);
  assert.match(transfer,/uint8_t packet\[496\]/);
  assert.match(transfer,/uint8_t chunk\[480\]/);
  assert.match(transfer,/request\.connection==connectionGeneration\.load\(\)/);
});
test('C3 recorder, safe sleep, transport fallback and OTA size remain unchanged',()=>{
  assert.match(ino,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(ino,/ODYSSEY_SD_DATA_FREQ_HZ=800000u/);
  assert.match(ino,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(ino,/streamSdWindow\(request\)/);
  assert.match(ino,/p\[16\]\|=1/);
  assert.doesNotMatch(transfer,/esp_vfs_fat_sdspi_mount|format_if_empty/);
});
