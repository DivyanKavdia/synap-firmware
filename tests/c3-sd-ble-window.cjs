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
  assert.match(transfer,/case 12:\s*case 30:[\s\S]*?streamSdWindow\(request\);[\s\S]*?sdBleTransferInFlight=false;[\s\S]*?continue;/);
  assert.match(transfer,/case 4:\s*error=readSelected\(request.path,request.offset,total,bytes,size\)/);
  assert.match(transfer,/case 16: error=OK; break;/);
  assert.match(transfer,/OdysseySdGuard guard/);
});
test('C3 notifications are paced, bounded, explicit-path, and MTU safe',()=>{
  const fn=transfer.split('static void streamSdWindow(const Request& request) {')[1]
    .split('static void worker(void*) {')[0];
  assert.match(fn,/capacity<496u/);
  assert.match(fn,/windowPath\[0\]/);
  assert.match(fn,/n<credits/);
  assert.match(fn,/readSelected\(windowPath,offset,total,chunk,length\)/);
  assert.match(fn,/notifySdWindow\(request,1,OK,fullSize,offset,chunk,length\)/);
  assert.match(fn,/vTaskDelay\(pacing\)/);
  assert.match(fn,/fast\?15u:20u/);
  assert.match(fn,/std::min\(uint8_t\(12\),request.credits\)/);
  assert.match(fn,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
  assert.match(fn,/odysseySdMarkVfsFailure\(\)/);
  assert.match(fn,/streamingEnabled\.load\(\) \|\| odysseyRecording\.load\(\)/);
  assert.match(transfer,/uint8_t packet\[496\]/);
  assert.match(transfer,/uint8_t chunk\[480\]/);
  assert.match(transfer,/request\.connection==connectionGeneration\.load\(\)/);
});
test('C3 recorder, safe sleep, transport fallback and OTA size remain unchanged',()=>{
  assert.match(ino,/alignas\(4\) static uint8_t batch\[ODYSSEY_SD_RECORD_BATCH_BYTES\]/);
  assert.match(ino,/ODYSSEY_SD_DATA_FREQ_HZ=2000000u/);
  assert.match(ino,/bool odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(ino,/streamSdWindow\(request\)/);
  assert.match(ino,/p\[16\]\|=1/);
  assert.doesNotMatch(transfer,/esp_vfs_fat_sdspi_mount|format_if_empty/);
});

test('C3 optional opcode31 tunes paced read-only windows without changing legacy 12/30 or deleting SD files',()=>{
 assert.match(caps,/p\[14\]=3; \/\/ Media-v3/);
 assert.match(transfer,/uint8_t operation=0,credits=6,paceMs=15;/);
 assert.match(transfer,/case 12:\s*case 30:\s*case 31:/);
 assert.match(transfer,/request\.operation==31u/);
 assert.match(transfer,/adaptive\?std::max\(uint8_t\(8\),std::min\(uint8_t\(15\),request\.paceMs\)\)/);
 assert.doesNotMatch(transfer,/if \(adaptive && n\+1u==credits\) return;/);
 assert.match(transfer,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
 assert.match(transfer,/const size_t pathStart=request\.compact\?16u:\(adaptive\?12u:\(fast\?11u:10u\)\)/);
 assert.match(transfer,/if \(adaptive\) request\.paceMs=std::max\(uint8_t\(8\),std::min\(uint8_t\(15\),p\[11\]\)\)/);
 assert.match(transfer,/if \(\(!ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED &&/);
 assert.match(transfer,/static constexpr bool ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
 assert.match(transfer,/const TickType_t pacing=pdMS_TO_TICKS\(paceMs\)/);
 const mode=transfer.split('static void streamSdWindow(const Request& request) {')[1]
   .split('static void worker(void*) {')[0];
 assert.doesNotMatch(mode,/unlink\(|fwrite\(|removeFile\(|odysseyRecoverSdCard\(/);
 assert.match(ino,/p\[14\]=3;/);
});

test('media-v3 compact SD window requires a connection-bound selected-file token',()=>{
 assert.match(caps,/p\[14\]=3;/);
 assert.match(transfer,/uint32_t connection=0,id=0,offset=0,sessionToken=0;/);
 assert.match(transfer,/static uint32_t selectedSessionToken=0,selectedSessionConnection=0;/);
 assert.match(transfer,/selectedSessionToken=error==OK\?request\.id:0;/);
 assert.match(transfer,/request\.compact=adaptive && length==16u;/);
 assert.match(transfer,/if \(request\.compact\) memcpy\(&request\.sessionToken,p\+12,4\);/);
 assert.match(transfer,/request\.sessionToken!=selectedSessionToken/);
 assert.match(transfer,/request\.connection!=selectedSessionConnection/);
 assert.match(transfer,/windowPath=selectedPath;/);
 assert.match(transfer,/readSelected\(windowPath,offset,total,chunk,length\)/);
 assert.match(transfer,/notifySdWindow\(request,2,FILE_UNAVAILABLE,0,request\.offset\)/);
 assert.match(transfer,/case 12:\s*case 30:\s*case 31:/);
 assert.match(transfer,/static constexpr bool ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
 assert.doesNotMatch(transfer.split('static void streamSdWindow(const Request& request) {')[1].split('static void worker(void*) {')[0],/unlink\(|fwrite\(|removeFile\(|format_if_empty/);
 assert.match(ino,/p\[14\]=3;/);
});


test('C3 quarantine blocks remount and explicit recovery after live CMD17/CMD18 timeout',()=>{
 const sd=load('firmware/shared/odyssey-sd-1631-detect.cpp');
 // The logged CMD17 timeout happened during an already mounted 4 KiB read.
 assert.match(sd,/static std::atomic<bool> odysseySdTimedOutReadQuarantined\{false\}/);
 assert.match(sd,/error==ESP_ERR_TIMEOUT &&\s*\(odysseySdBootState\.load\(\)==1 \|\| odysseySdNativeCard!=nullptr\)/);
 assert.match(sd,/cmd->opcode==17u \|\| cmd->opcode==18u/);
 assert.match(sd,/odysseySdTimedOutReadQuarantined=true;/);
 // Validation can time out *after* native mount but before bootState=ready.
 const validateFailure=sd.split('if (!odysseySdValidateVfsLocked(reason,attempt)) {')[1]
   .split('// Persist the field C3+SD divider')[0];
 assert.match(validateFailure,/odysseySdTimedOutReadQuarantined\.load\(\)/);
 assert.match(validateFailure,/no SPI teardown or retry/);
 assert(validateFailure.indexOf('odysseySdTimedOutReadQuarantined.load()')<
   validateFailure.indexOf('odysseySdReleaseLocked()'),'validation timeout quarantine before unmount');
 assert.match(sd,/stop mount attempts: timed-out SD read\/write requires rail power cycle/);
 assert.match(sd,/odysseySdUnsafeToSleep=true;/);
 const mounted=sd.split('static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {')[1]
   .split('void odysseyDetectSdCard()')[0];
 assert.match(mounted,/odysseySdTimedOutReadQuarantined\.load\(\)/);
 assert.match(mounted,/return false;/);
 const recovery=sd.split('bool odysseyRecoverSdCard(const char* reason) {')[1]
   .split('// Explicit media-v1 operation 19')[0];
 assert.match(recovery,/odysseySdReadTimeoutQuarantined\(\)/);
 assert.match(recovery,/SD rail power-cycle required; recordings retained/);
 assert.match(sd,/odysseySdTimedOutReadQuarantined=false;  \/\/ never cleared by an ordinary BLE read/);
 assert.match(sd,/mount\.format_if_mount_failed=false/);
 assert.match(ino,/odysseySdTimedOutReadQuarantined=true;/);
});

test('SD read-timeout telemetry remains RAM-only, compact and preserves legacy fields',()=>{
 assert.match(transfer,/case 29: \{/);
 assert.match(transfer,/odysseySdReadTimeoutQuarantined\(\)\?16u:0u/);
 assert.match(transfer,/odysseySdWriteTimeoutQuarantined\(\)\?32u:0u/);
 assert.match(transfer,/readFlags/);
 const section=transfer.split('case 29: {')[1].split('case 4:')[0];
 assert.doesNotMatch(section,/fopen\(|fread\(|unlink\(|fwrite\(|odysseyRecoverSdCard\(/);
 assert.match(ino,/odysseySdReadTimeoutQuarantined\(\)/);
});


test('C3 selected WAV metadata avoids repeated FAT stats without weakening read checks',()=>{
 const transfer=load('firmware/shared/odyssey-sd-1631-transfer.cpp');
 assert.match(transfer,/static uint32_t selectedFileSize=0;/);
 assert.match(transfer,/selectedFileSize=0;/);
 assert.match(transfer,/selectedFileSize=total;/);
 const read=transfer.split('static uint8_t readWavChunk(')[1].split('static uint8_t readSelected(')[0];
 assert.match(read,/const bool selectedMetadataValid=selectedSessionToken!=0/);
 assert.match(read,/selectedSessionConnection==generation/);
 assert.match(read,/strcmp\(logical,selectedPath\)==0/);
 assert.match(read,/if \(selectedMetadataValid\) \{\s*total=selectedFileSize;/);
 assert.match(read,/if \(stat\(full,&st\)!=0\)/);
 assert.match(read,/const size_t step=std::min\(size_t\(512\),want-got\)/);
 assert.match(read,/const int closeResult=fclose\(file\)/);
 assert.match(read,/if \(!seekOk \|\| got!=want \|\| closeResult!=0\)/);
 assert.match(transfer,/ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
 assert(ino.includes(transfer),'release sketch must exactly include the source with selected metadata guard');
});
