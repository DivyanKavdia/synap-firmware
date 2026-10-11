'use strict';
const { test }=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=file=>fs.readFileSync(path.join(root,file),'utf8');
const shared=file=>read('firmware/shared/'+file);
const compiled=read('synap_esp32s3/synap_esp32s3.ino');
const active=require('../firmware/shared/sources.json');

test('C3 contract tests track the actual assembled 1631 production runtime, not retired sources',()=>{
  const selected=['odyssey-sd-1631-detect.cpp','odyssey-sd-1631-recording.cpp',
    'odyssey-sd-1631-transfer.cpp','odyssey-c3-wifi.cpp','ble-control.cpp',
    'audio-session.cpp','battery.cpp','power.cpp','boot.cpp'];
  for(const path of selected){
    assert(active.includes(path),'active production source list must contain '+path);
    assert(compiled.includes(shared(path).trim()),'assembled production source is stale for '+path);
  }
  assert(!active.includes('odyssey-sd-detect.cpp'));
  assert(!active.includes('odyssey-sd-recording.cpp'));
  assert(!active.includes('odyssey-sd-transfer.cpp'));
});

test('C3 SD 1 MHz guarded boot precedes BLE; no silent auto-format',()=>{
  const boot=shared('boot.cpp'),sd=shared('odyssey-sd-1631-detect.cpp');
  assert.match(sd,/ODYSSEY_SD_DATA_FREQ_HZ=800000u/);
  assert.match(sd,/ODYSSEY_SD_BOOT_ATTEMPTS=1/);
  assert.match(sd,/odysseySdValidateVfsLocked\(reason,attempt\)/);
  assert.match(sd,/esp_vfs_fat_sdspi_mount\(ODYSSEY_SD_MOUNT_POINT,/);
  assert.doesNotMatch(sd,/format_if_mount_failed\s*=\s*true|format_if_empty\s*=\s*true/);
  assert(boot.indexOf('odysseyInitializeSdCardBeforeBle();')<boot.indexOf('initializeBLE();'));
});

test('offline double tap owns SD WAV and BLE START waits for recorder finalize',()=>{
  const take=shared('odyssey-sd-1631-recording.cpp'),control=shared('ble-control.cpp'),
    session=shared('audio-session.cpp'),power=shared('power.cpp');
  assert.match(take,/alignas\(4\) static uint8_t batch\[4096\]/);
  assert.match(take,/OdysseySdGuard storage;/);
  assert.match(take,/odysseyWavHeader\(header,0\)/);
  assert.match(take,/fclose\(file\)/);
  assert.match(take,/odysseyPersistRecordFailure\(0,0\)/);
  assert.match(take,/odysseyRecording=true;/);
  assert.match(take,/odysseyPrepareForConnectedStreaming\(uint32_t timeoutMs\)/);
  assert.match(session,/odysseyPrepareForConnectedStreaming\(1500u\)/);
  assert.match(control,/A BLE connection alone must never stop an SD-owned recording/);
  assert.match(power,/odysseyToggleRecording\(\)/);
});

test('BLE and SD media safely resume by ID and offset; never delete on transfer',()=>{
  const media=shared('odyssey-sd-1631-transfer.cpp'),
    cap=shared('module-capabilities.cpp');
  assert.match(media,/request\.connection!=connectionGeneration\.load\(\)/);
  assert.match(media,/request\.operation==25/);
  assert.match(media,/readSelected\(request\.path,offset,total,chunk,length\)/);
  assert.match(media,/notifySdWindow\(request,2,OK,fullSize,offset\)/);
  assert.match(media,/case 4:\s*error=readSelected\(request\.path,request\.offset,total,bytes,size\)/);
  assert.match(media,/case 17: error=removeFile\(request\.path\)/);
  assert.match(media,/case 18:/);
  assert.match(media,/if \(count>=100\) break;/);
  assert.match(media,/uint32_t\(millis\(\)-last\)<15000u/);
  assert.match(cap,/p\[16\]\|=1/);
  assert.match(cap,/if \(OdysseyWifi::available\(\)\) p\[16\]\|=2/);
});

test('C3 connected BLE uses mbuf-saving ADPCM, central-managed connection timing',()=>{
  const ble=shared('ble-control.cpp'),session=shared('audio-session.cpp'),
    power=shared('cpu-power.cpp');
  assert.match(session,/#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU\s+pcmTransport=false;/);
  assert.match(ble,/\(void\)desc;[\s\S]*?#else\s+if\(desc\)server->updateConnParams/);
  assert.match(power,/active=active \|\| deviceConnected\.load\(\) \|\| reconnectWindow/);
  assert.match(ble,/bleServer->startAdvertising\(\)/);
});

test('Wi-Fi is advertised only if enabled and the shipped C3 feature defaults off',()=>{
  const wifi=shared('odyssey-c3-wifi.cpp'),
    cap=shared('module-capabilities.cpp');
  assert.match(wifi,/#define SYNAP_C3_WIFI_UPLOAD_ENABLED 0/);
  assert.match(wifi,/#if SYNAP_C3_WIFI_UPLOAD_ENABLED/);
  assert.match(cap,/if \(OdysseyWifi::available\(\)\) p\[16\]\|=2/);
});

test('SD failure vetoes unsafe sleep; firmware battery cannot claim an invalid full charge',()=>{
  const sd=shared('odyssey-sd-1631-detect.cpp'),power=shared('power.cpp'),
    battery=shared('battery.cpp');
  assert.match(sd,/odysseySdUnsafeToSleep=true/);
  assert.match(sd,/odysseyPrepareSdForPowerTransition\(uint32_t timeoutMs\)/);
  assert.match(power,/odysseySdUnsafeToSleep\.load\(\)/);
  assert.match(power,/odysseyPrepareSdForPowerTransition\(1000u\)/);
  assert.match(battery,/cellMv>=2800u && cellMv<=4350u/);
  assert.match(battery,/batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0/);
});
