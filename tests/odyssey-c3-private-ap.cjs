'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const ap=fs.readFileSync('firmware/shared/odyssey-c3-sd-ap.cpp','utf8');
const detector=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const wifi=fs.readFileSync('firmware/shared/odyssey-c3-wifi.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const sources=JSON.parse(fs.readFileSync('firmware/shared/sources.json','utf8'));
test('only enhanced C3 compiles a protected temporary WPA2 access point',()=>{
  assert(sources.includes('odyssey-c3-sd-ap.cpp'));
  assert.match(ap,/#if SYNAP_C3_WIFI_UPLOAD_ENABLED/);
  assert.match(ap,/WiFi\.mode\(WIFI_AP\)/);
  assert.match(ap,/WiFi\.softAP\(next\.ssid,next\.password,1,false,1\)/);
  assert.match(ap,/esp_fill_random\(randomBytes,sizeof\(randomBytes\)\)/);
  assert.match(ap,/WebServer\(80\)/);
  assert.match(ap,/IDLE_MS=180000u, MAX_MS=900000u/);
  assert.match(wifi,/uploadBusy\.load\(\) \|\| OdysseyWifiAp::busy\(\)/);
  assert.match(ino,/namespace OdysseyWifiAp/);
});
test('C3 AP serves SD exclusively read only, with logical WAV headers and no destructive paths',()=>{
  assert.match(ap,/OdysseySdGuard sd\(pdMS_TO_TICKS\(1500\)\)/);
  assert.match(ap,/fopen\(full,"rb"\)/);
  assert.match(ap,/fseek\(input,44,SEEK_SET\)/);
  assert.match(ap,/virtualHeader\(header,fileSize\)/);
  assert.match(ap,/setContentLength\(fileSize\)/);
  assert.match(ap,/safeWav\(path\.c_str\(\)\)/);
  assert.match(ap,/Content-Disposition/);
  assert.match(ap,/Cache-Control/);
  assert.match(ap,/Content-Security-Policy/);
  assert.match(ap,/server->client|apServer->client/);
  assert.doesNotMatch(ap,/\bfwrite\s*\(|\bunlink\s*\(|\bremove\s*\(|\bmkdir\s*\(|\brename\s*\(|sdcard_format|sdspi_mount|odysseyRecoverSdCard/);
  assert.doesNotMatch(ap,/\bPreferences\b|WiFi\.begin\s*\(/);
});
test('C3 AP operations stay outside existing busy gate and retain BLE OTA exclusion',()=>{
  const worker=transfer.split('static void worker(void*) {')[1];
  assert(worker);
  assert(worker.indexOf('request.operation==21')<worker.indexOf('if (odysseyRecording.load() || streamingEnabled.load() || OdysseyWifi::busy()'));
  assert(worker.indexOf('request.operation==22')<worker.indexOf('if (odysseyRecording.load() || streamingEnabled.load() || OdysseyWifi::busy()'));
  assert.match(worker,/case 20: error=OdysseyWifiAp::begin\(\)\?OK:BUSY/);
  assert.match(worker,/OdysseyWifiAp::encode\(detail,sizeof\(detail\)\)/);
  assert.match(worker,/OdysseyWifiAp::stop\(\)\?OK:BUSY/);
});
test('expected SDIO CMD52 probe timeout is ignored by first-real-fault interceptor',()=>{
  for(const src of [detector,ino]){
    assert.match(src,/cmd->opcode==52u/);
    assert.match(src,/error==ESP_ERR_TIMEOUT \|\| error==ESP_ERR_NOT_SUPPORTED \|\| error==ESP_ERR_INVALID_CRC/);
    assert.match(src,/if \(cmd && error!=ESP_OK && !expectedIoProbe\)/);
    assert.match(src,/sdspi_host_do_transaction\(slot,cmd\)/);
  }
});
test('legacy station credential save verifies NVS readback without exposing passwords',()=>{
  assert.match(wifi,/readCredentials\(verifySsid,sizeof\(verifySsid\),/);
  assert.match(wifi,/strcmp\(password,verifyPassword\)==0/);
  assert.doesNotMatch(wifi,/Serial\.printf\([^)]*password/);
});

test('private C3 page optionally sends bounded WAV chunks to a known HTTPS PWA opener',()=>{
  for(const src of [ap,ino]){
    assert.match(src,/const channel=query\.get\('bridge'\)/);
    assert.match(src,/const target=query\.get\('origin'\)/);
    assert.match(src,/window\.opener\.postMessage/);
    assert.match(src,/event\.origin!==target/);
    assert.match(src,/event\.source!==window\.opener/);
    assert.match(src,/16384/);
    assert.match(src,/await expect\('begin'/);
    assert.match(src,/await expect\('chunk'/);
    assert.match(src,/await expect\('end'/);
    assert.match(src,/fetch\(link\.href,\{cache:'no-store'\}\)/);
    assert.match(src,/if\(!link \|\| !enabled \|\| !ready\)return/);
    assert.match(src,/href='\/file\?key=/);
  }
  assert.match(ap,/script-src 'unsafe-inline'; connect-src 'self'/);
  assert.match(ap,/if\(remaining \|\| sent!=44\)client\.stop\(\)/);
  assert.doesNotMatch(ap,/\bfwrite\s*\(|\bunlink\s*\(|\bremove\s*\(|sdcard_format|esp_vfs_fat_sdspi_mount/);
});
