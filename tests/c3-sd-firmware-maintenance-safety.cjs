'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('shipping C3 BLE SD maintenance is fail-closed BEFORE any FAT operation',()=>{
  assert.match(tx,/static constexpr bool ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
  const worker=tx.split('static void worker(void*) {')[1].split('class CommandCallbacks :')[0];
  const guard=worker.indexOf('if ((!ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED &&');
  const mutation=worker.indexOf('case 17:');
  assert(guard>=0 && mutation>guard,'reject destructive commands before worker enters switch');
  for(const op of [17,18])
    assert(worker.slice(guard,mutation).includes('request.operation=='+op+'u'),'block media operation '+op);
  const refusal=worker.slice(guard,worker.indexOf('sdBleTransferInFlight=true;',guard));
  assert.match(refusal,/reply\(request,BAD_COMMAND\)/);
  assert.match(refusal,/continue;/);
  assert(!/odysseyRecoverSdCard|unlink\(|clearRecordings\(|odysseyFormatSdCard\(/.test(refusal),
    'fail closed without reaching FAT mutations or recovery');
  assert.match(tx,/case 4:/,'read-only SD sync remains');
  assert.match(tx,/case 7:/,'read-only SD catalogue remains');
  assert.match(tx,/case 32:/,'read-only SD FAT capacity remains');
  assert.match(tx,/ODYSSEY_C3_SD_EXPLICIT_FORMAT_ENABLED=true/);
  assert.match(tx,/request.offset!=ODYSSEY_C3_SD_FORMAT_TOKEN/);
  assert.match(tx,/recordedFault>=40u && recordedFault!=48u/);
  assert.match(ino,/ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
});

test('live op29 remains RAM-only and reports the first physical SDSPI command fault',()=>{
  const op29=tx.split('case 29: {')[1].split('case 4:')[0];
  assert.match(op29,/char detail\[480\]/);
  for(const key of ['wireCmd','wireErr','wireArg','wireMs'])
    assert(op29.includes('\\"'+key+'\\"'),'missing '+key);
  for(const func of ['odysseySdWireFaultCmd','odysseySdWireFaultError',
    'odysseySdWireFaultArg','odysseySdWireFaultElapsedMs'])
    assert(op29.includes(func+'()'),'missing '+func);
  assert.doesNotMatch(op29,/fopen\(|fwrite\(|unlink\(|esp_vfs_fat_sdcard_format\(|sdmmc_read_sectors\(|odysseyRecoverSdCard\(/);
  // Maximum possible JSON reply (all uint32 counters at UINT32_MAX).
  const keys=['version','sdState','sdLiveProbe','ioOp','ioStep','ioErr','reads',
    'cacheHits','diskFills','lastRequestedOffset','lastOkOffset','faultOffset',
    'windowRequests','mtuFallbacks','wireCmd','wireErr','wireArg','wireMs'];
  const max=Object.fromEntries(keys.map(k=>[k,k==='version'?1:k==='ioErr'?-2147483648:4294967295]));
  assert(Buffer.byteLength(JSON.stringify(max))<480,'media payload must fit 480 bytes plus 16-byte header');
});

test('C3 capacity is guarded, observational, and excludes unmounted FAT reads',()=>{
 const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
 const capacity=detect.split('esp_err_t odysseySdGetCapacity(')[1].split('// Operator-initiated, once-per-boot')[0];
 assert.match(capacity,/OdysseySdGuard guard/);
 assert.match(capacity,/!odysseySdReady\(\)/);
 assert.match(capacity,/esp_vfs_fat_info\(ODYSSEY_SD_MOUNT_POINT/);
 assert.doesNotMatch(capacity,/format\(|fopen\(|fwrite\(|unlink\(|odysseySdMountLocked\(/);
});
