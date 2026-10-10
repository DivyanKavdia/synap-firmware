'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const read=p=>fs.readFileSync(p,'utf8');
const sd=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const ota=read('firmware/shared/ota.cpp');
const boot=read('firmware/shared/boot.cpp');
const runtime=read('firmware/shared/runtime.cpp');

test('committed C3 OTA restarts when card unmounted despite persisted SD failure',()=>{
  const block=sd.split('bool odysseyPrepareSdForCommittedOtaRestart(uint32_t timeoutMs) {')[1].split('#else')[0];
  assert(block);
  assert.match(block,/OdysseySdGuard guard\(pdMS_TO_TICKS\(timeoutMs\)\)/);
  assert.match(block,/if \(!guard \|\| odysseyRecording\.load\(\)\)/);
  const notReady=block.split('if (!ready) {')[1].split('return true;')[0];
  assert.match(notReady,/odysseySdReleaseLocked\(\)/);
  assert.doesNotMatch(notReady,/odysseySdUnsafeToSleep\.load\(\).*return false;/);
  assert.match(block,/odysseySdQuiesceLocked\(ODYSSEY_SD_QUIESCE_BUDGET_MS\)/);
  assert.match(block,/if \(idle!=1\) \{[\s\S]*?return false;/);
  assert.match(ota,/if \(!odysseyPrepareSdForCommittedOtaRestart\(1000u\)\) return;/);
  assert.match(runtime,/bool odysseyPrepareSdForCommittedOtaRestart\(uint32_t timeoutMs\);/);
  const ordinary=sd.split('bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {')[1].split('bool odysseyPrepareSdForCommittedOtaRestart')[0];
  assert.match(ordinary,/if \(odysseySdUnsafeToSleep\.load\(\)\) \{[\s\S]*?return false;/);
  assert.match(ordinary,/if \(idle!=1\) \{[\s\S]*?return false;/);
});

test('partition state is traced on boot and commit without bypassing ESP-IDF image checks',()=>{
  assert.match(boot,/esp_ota_get_running_partition\(\)/);
  assert.match(boot,/esp_ota_get_boot_partition\(\)/);
  assert.match(boot,/esp_ota_get_state_partition\(runningOta,&otaImageState\)/);
  assert.match(ota,/esp_ota_set_boot_partition\(target\)/);
  assert.match(ota,/\[OTA\] commit result=/);
  assert.match(boot,/\[OTA\] boot /);
});

test('C3 SD boot mount is read-verified without FAT writes or battery calibration admission',()=>{
  const body=sd.split('static bool odysseySdValidateVfsLocked(')[1]
    .split('static uint8_t odysseySdMountReasonCode')[0];
  assert.match(body,/stat\(ODYSSEY_SD_MOUNT_POINT,&root\)/);
  assert.match(body,/stat\(ODYSSEY_SD_RECORDING_DIR,&recordings\)/);
  assert.match(body,/opendir\(ODYSSEY_SD_RECORDING_DIR\)/);
  assert.match(body,/closedir\(verified\)/);
  assert.doesNotMatch(body,/fputc\(|fflush\(|unlink\(|mkdir\(|fopen\(|fsync\(|odysseySdPowerSafe/);
  assert.doesNotMatch(sd,/SD\.format\(/);
});
