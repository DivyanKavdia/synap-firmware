'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

test('legacy C3 single-file deletion stays gated by mount/timeout, not battery ADC',()=>{
  const source=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
  const method=source.split('static uint8_t removeFile(const char* path) {')[1]
    .split('static uint16_t clearRecordings()')[0];
  assert.match(method,/safeWavPath\(path\)/);
  assert.doesNotMatch(method,/batteryAvailable|batteryMillivolts|odysseySdPowerSafe/);
  assert.doesNotMatch(method,/if \(!odysseySdPowerSafe\(/);
  assert.match(method,/OdysseySdGuard guard/);
  assert.match(method,/!storageReady\(\)/);
  assert.match(method,/if \(unlink\(full\)!=0\) \{/);
  assert.match(method,/if \(errno==ENOENT\) return OK/);
  assert.match(method,/removeFailureStep=2/);
  assert.doesNotMatch(method.split('if (unlink(full)!=0)')[1],/stat\(|unlink\(|odysseyRecoverSdCard/);
  assert.match(method,/odysseySdWriteTimeoutQuarantined\(\)/);
  assert.match(method,/odysseySdReadTimeoutQuarantined\(\)/);
  assert.match(source,/case 17:\s*[\s\S]*?error=removeFile\(request\.path\)/);
  const clear=source.split('case 18:')[1].split('case 19:')[0];
  assert.doesNotMatch(clear,/odysseySdPowerSafe|batteryMillivolts/);
});

test('field CMD24 failure blocks BLE unlink on all clients before any FAT access',()=>{
  const source=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
  const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
  for (const code of [source,ino]) {
    assert.match(code,/ODYSSEY_C3_SD_BLE_SINGLE_DELETE_ENABLED=false/);
    assert.match(code,/ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED=false/);
    const worker=code.split('static void worker(void*) {')[1].split('case 3:')[0];
    assert.match(worker,/!ODYSSEY_C3_SD_BLE_SINGLE_DELETE_ENABLED && request\.operation==17u/);
    assert.match(worker,/reply\(request,BAD_COMMAND\)/);
    assert(worker.indexOf('request.operation==17u') < worker.indexOf('sdBleTransferInFlight=true'),
      'no SD mutex, FAT stat or unlink before the op17 veto');
  }
});
