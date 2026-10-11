'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');

test('Native C3 SD disables extra FatFs CMD13 readiness probes but keeps native driver',()=>{
  assert.match(sd,/mount\.disk_status_check_enable=false/);
  assert.match(sd,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT,/);
  assert.match(sd,/mount\.format_if_mount_failed=false/);
  assert.match(sd,/esp_err_t odysseyFormatSdCard\(\)/);
  assert.equal((sd.match(/esp_vfs_fat_sdcard_format\(/g)||[]).length,1,'format only in explicit op19 backend');
});

test('Catalogue read retries at most once without repeating writes/remounts',()=>{
  const block=tx.split('case 7:\n        error=catalogue(total);')[1].split('case 8:')[0];
  assert.match(block,/catalogueErrno==EIO \|\| catalogueErrno==ENODEV/);
  assert.match(block,/vTaskDelay\(pdMS_TO_TICKS\(120u\)\)/);
  assert.match(block,/error=catalogue\(total\)/);
  assert.match(block,/if \(error==IO_ERROR\)/);
  assert.match(block,/odysseySdMarkVfsFailure\(\)/);
  assert.doesNotMatch(block,/unlink\(|removeFile\(|odysseyRecoverSdCard\(|odysseySdReleaseLocked\(|esp_vfs_fat_sdspi_mount\(/);
});

test('Only read operation has transient EIO retry; deletion is never automatically retried',()=>{
  const del=tx.split('case 17:')[1].split('case 18:')[0];
  assert.match(del,/error=removeFile\(request\.path\);/);
  assert.match(del,/noteSdMediaFault\(17,removeFailureStep,removeErr\)/);
  assert.doesNotMatch(del,/vTaskDelay\(|while\s*\(|retry|error=removeFile\(request\.path\);[\s\S]*error=removeFile\(/);
});
