'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const record=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');

test('first ambiguous CMD24/CMD25 failure cuts off every subsequent physical command',()=>{
  const callback=detect.split('host.do_transaction=[](int slot,sdmmc_command_t* cmd)->esp_err_t {')[1]
    .split('sdspi_device_config_t device=')[0];
  assert(callback, 'native SDSPI host intercept must exist');
  const barrier='if (odysseySdAmbiguousWrite.load()) return ESP_ERR_INVALID_STATE;';
  assert(callback.includes(barrier));
  assert(callback.indexOf(barrier)<callback.indexOf('sdspi_host_do_transaction(slot,cmd)'),
    'guard must precede the only physical command');
  assert.equal((callback.match(/sdspi_host_do_transaction\(slot,cmd\)/g)||[]).length,1);
  assert.match(callback,/cmd->opcode==24u \|\| cmd->opcode==25u/);
  assert.match(callback,/cmd->opcode==13u && odysseyWriteStatusChecksOutstanding.load\(\)!=0/);
  assert.match(callback,/odysseyWriteStatusChecksOutstanding=2;/);
  assert.match(callback,/if \(pending\) odysseyWriteStatusChecksOutstanding=uint8_t\(pending-1u\)/);
  assert.match(callback,/odysseyWriteStatusChecksOutstanding=0;/);
  assert.match(callback,/odysseySdAmbiguousWrite=true;/);
  assert.match(callback,/odysseySdUnsafeToSleep=true;/);
  assert.doesNotMatch(callback,/vTaskDelay|delay\(|spi_device_transmit|sdmmc_send_cmd\(|odysseySdBitBangRecoverLocked/);
});

test('SD error latch is never cleared by resetting diagnostics or a new take',()=>{
  const declarations=detect.split('void odysseySdClearWireFault()')[0];
  assert.match(declarations,/static std::atomic<bool> odysseySdAmbiguousWrite\{false\}/);
  const clear=detect.split('void odysseySdClearWireFault() {')[1].split('uint32_t odysseySdWireFaultCmd()')[0];
  assert.doesNotMatch(clear,/odysseySdAmbiguousWrite|odysseyWriteStatusChecksOutstanding/);
  const take=record.split('static void odysseyRecordTake() {')[1]
    .split('static void odysseyRecordTask(void*) {')[0];
  assert.match(take,/odysseySdClearWireFault\(\)/);
  assert.doesNotMatch(take,/odysseySdAmbiguousWrite=false/);
});

test('neither explicit recovery nor failed native mount reaches GPIO-level recovery',()=>{
  const begin=detect.split('static bool odysseySdBeginLocked() {')[1]
    .split('static bool odysseySdValidateVfsLocked')[0];
  const mount=detect.split('static bool odysseySdMountOnceLocked(')[1]
    .split('static bool odysseySdMountLocked(')[0];
  const recovery=detect.split('bool odysseyRecoverSdCard(const char* reason) {')[1]
    .split('bool odysseyPrepareSdForPowerTransition')[0];
  assert.match(begin,/if \(odysseySdAmbiguousWrite.load\(\)\)[\s\S]*?return false/);
  const veto=mount.split('if (odysseySdAmbiguousWrite.load()) {')[1].split('odysseySdBootState=0;')[0];
  assert.match(veto,/return false;/);
  assert(mount.indexOf('if (odysseySdAmbiguousWrite.load())')<mount.indexOf('odysseySdReleaseLocked()'));
  assert(recovery.indexOf('if (odysseySdAmbiguousWrite.load())')<
    recovery.indexOf('odysseySdControllerBusyLow()'));
  assert.match(recovery,/real SD power reset required/);
});

test('mount and recording safety invariants remain intact',()=>{
  assert.match(detect,/mount.format_if_mount_failed=false/);
  assert.match(detect,/mount.disk_status_check_enable=false/);
  assert.match(detect,/ODYSSEY_SD_DATA_FREQ_HZ=800000u/);
  assert.doesNotMatch(detect,/esp_vfs_fat_sdcard_format\(/);
  assert.match(record,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=10000u/);
  assert.match(record,/fwrite\(batch,1,sizeof\(batch\),file\)/);
  assert.match(record,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
  assert.doesNotMatch(record,/remove\(|unlink\(|esp_vfs_fat_sdcard_format\(/);
});
