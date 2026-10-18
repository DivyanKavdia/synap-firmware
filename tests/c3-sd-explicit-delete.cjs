'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

test('C3 explicit SD delete permits untrusted ADC but blocks trusted low voltage',()=>{
  const source=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
  const method=source.split('static uint8_t removeFile(const char* path) {')[1]
    .split('static uint16_t clearRecordings()')[0];
  assert.match(method,/safeWavPath\(path\)/);
  assert.match(method,/batteryAvailable\.load\(\) && batteryMillivolts<ODYSSEY_SD_WRITE_CONTINUE_MIN_MV/);
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
  assert.match(source,/case 18:[\s\S]*odysseySdPowerSafe/,'bulk delete stays conservatively gated');
});
