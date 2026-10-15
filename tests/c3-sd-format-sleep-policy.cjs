'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const read=(file)=>fs.readFileSync(file,'utf8');
const sd=read('firmware/shared/odyssey-sd-1631-detect.cpp');
const tx=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
const rec=read('firmware/shared/odyssey-sd-1631-recording.cpp');
const cap=read('firmware/shared/module-capabilities.cpp');
const power=read('firmware/shared/power.cpp');
const led=read('firmware/shared/status-led.cpp');
const runtime=read('firmware/shared/runtime.cpp');
const ota=read('firmware/shared/ota.cpp');
const ino=read('synap_esp32s3/synap_esp32s3.ino');

test('only explicitly confirmed operation 19 formats the already mounted FAT card',()=>{
  assert.match(tx,/case 19:\s*\{/);
  assert.match(tx,/odysseyFormatSdCard\(\)/);
  assert.match(sd,/esp_err_t odysseyFormatSdCard\(\)/);
  assert.equal((sd.match(/esp_vfs_fat_sdcard_format\s*\(/g)||[]).length,1);
  assert.match(sd,/if \(!guard \|\| !odysseySdReady\(\) \|\| !odysseySdNativeCard/);
  assert.match(sd,/odysseySdFormatInProgress\.exchange\(true\)/);
  assert.match(sd,/odysseySdValidateVfsLocked\("format",1\)/);
  assert.match(sd,/mount\.format_if_mount_failed=false/);
  assert.doesNotMatch(rec,/esp_vfs_fat_sdcard_format\s*\(/);
  assert.match(tx,/if \(!storageReady\(\)\) \{ error=NO_SD;break; \}/);
  assert.match(cap,/p\[16\]\|=4/);
});

test('offline C3+SD is awake for touch gestures, but SD-less C3 and manual sleep remain',()=>{
  assert.match(power,/if \(odysseyRecording\.load\(\)\) \{[\s\S]*?return;\s*\}/);
  assert.match(power,/if \(odysseySdReady\(\) \|\| odysseySdFormatting\(\)\) return;/);
  assert.match(power,/enterDeepSleep\("disconnected-timeout"\)/);
  assert.match(power,/enterDeepSleep\("touch-hold"\)/);
  assert.match(sd,/power transition deferred: local recording or format active/);
  assert.match(runtime,/bool odysseySdFormatting\(\);/);
  assert.match(ota,/odysseyRecording\.load\(\) \|\| odysseySdFormatting\(\)/);
  assert.match(rec,/odysseySdFormatting\(\) \|\| otaBusy\(\)/);
});

test('purple remains visible until WAV finalization completes; verified recorder writer stays intact',()=>{
  assert.match(led,/const bool finalizing=odysseyStopRequested\.load\(\)/);
  assert.match(led,/finalizing\?550u:1800u/);
  assert.doesNotMatch(led,/!odysseyStopRequested\.load\(\) && phase<260u/);
  assert.match(power,/trusted critical battery requested offline SD STOP/);
  assert.match(rec,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(rec,/odysseyCheckpointWav\(file,checkpointErrno\)/);
  assert.match(rec,/fwrite\(batch,1,sizeof\(batch\),file\)/);
  for(const part of [sd,tx,rec,cap,power,led]) assert.ok(ino.includes(part),'generated sketch must include changed shared module');
  assert.match(ino,/bool odysseySdFormatting\(\); \/\/ media-v1 op19/);
});
