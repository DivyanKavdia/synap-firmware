'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const wifi=read('firmware/shared/odyssey-c3-wifi.cpp');
const transfer=read('firmware/shared/odyssey-sd-1631-transfer.cpp');
const caps=read('firmware/shared/module-capabilities.cpp');
const sources=JSON.parse(read('firmware/shared/sources.json'));
const power=read('firmware/shared/power.cpp');
const cpu=read('firmware/shared/cpu-power.cpp');
const ota=read('firmware/shared/ota.cpp');
const recorder=read('firmware/shared/odyssey-sd-1631-recording.cpp');

test('Odyssey C3 Wi-Fi uses STA HTTPS upload and never exposes an insecure SoftAP transport',()=>{
  assert.ok(sources.includes('odyssey-c3-wifi.cpp'));
  assert.match(wifi,/#include <WiFi\.h>/);
  assert.match(wifi,/WiFi\.mode\(WIFI_STA\)/);
  assert.doesNotMatch(wifi,/WIFI_AP|softAP\s*\(/);
  assert.match(wifi,/strncmp\(next\.endpoint,"https:\/\/",8\)/);
  assert.match(wifi,/GOOGLE_TRUST_ROOT_R1/);
  assert.match(wifi,/SHA-256: D9:47:43:2A:BD:E7:B7:FA/);
  assert.match(wifi,/tls\.setCACert\(GOOGLE_TRUST_ROOT_R1\)/);
  assert.match(wifi,/configTime\(0,0,"time\.google\.com","time\.cloudflare\.com","pool\.ntp\.org"\)/);
  assert.match(wifi,/time\(nullptr\)<TLS_MIN_UNIX_TIME/);
  assert.doesNotMatch(wifi,/tls\.useBuiltinCACertBundle\s*\(/);
  assert.doesNotMatch(wifi,/setInsecure\s*\(/);
});

test('C3 Wi-Fi credentials are provisioned over BLE and persisted without exposing password in status',()=>{
  assert.match(wifi,/prefs\.begin\("c3-wifi",false\)/);
  assert.match(wifi,/putString\("ssid",ssid\)/);
  assert.match(wifi,/putString\("pass",password\)/);
  assert.match(transfer,/case 23:/);
  assert.match(transfer,/OdysseyWifi::configChunk\(request\.offset,request\.path\)/);
  assert.match(transfer,/case 24:/);
  assert.match(transfer,/OdysseyWifi::applyConfig\(request\.offset\)/);
  assert.match(transfer,/request\.operation==25/);
  assert.match(transfer,/case 26:/);
  assert.match(caps,/p\[16\]\|=2/);
  assert.doesNotMatch(wifi,/\"password\"/);
});

test('C3 Wi-Fi upload uses minimal verified HTTP over TLS and finalizes through scoped cloud device-upload API',()=>{
  assert.match(wifi,/SEGMENT_MS=120000u/);
  assert.match(wifi,/PCM_BYTES_PER_MS=32u/);
  assert.match(wifi,/fseek\(file,long\(44u\+pcmOffset\),SEEK_SET\)/);
  assert.match(wifi,/WiFiClientSecure tls/);
  assert.match(wifi,/tls\.connect\(host,port,15000\)/);
  assert.match(wifi,/PUT \/v1\/device-uploads\/%s\/segments\/%lu HTTP\/1\.1/);
  assert.match(wifi,/POST \/v1\/device-uploads\/%s\/finalize HTTP\/1\.1/);
  assert.match(wifi,/Authorization: SynapDevice %s/);
  assert.match(wifi,/wavHeader\(wav,pcmBytes\)/);
  assert.match(wifi,/readHttpStatus\(tls\)/);
  assert.doesNotMatch(wifi,/#include <HTTPClient\.h>|\bHTTPClient\b/,'C3 must not link the heavyweight Arduino HTTPClient wrapper');
  assert.doesNotMatch(wifi,/\bunlink\s*\(|removeFile|clearRecordings/,'Wi-Fi upload must never delete the SD source');
});

test('Wi-Fi upload owns SD and blocks recording OTA sleep and CPU downclock',()=>{
  assert.match(wifi,/OdysseySdGuard storage\(pdMS_TO_TICKS\(5000\)\)/);
  assert.match(recorder,/OdysseyWifi::busy\(\)/);
  assert.match(transfer,/OdysseyWifi::busy\(\)/);
  assert.match(power,/odysseyRecording\.load\(\) \|\| OdysseyWifi::busy\(\)/);
  assert.match(power,/if \(OdysseyWifi::busy\(\) \|\| OdysseyTransfer::busy\(\)\) return/);
  assert.match(cpu,/active=active \|\| odysseyRecording\.load\(\) \|\| OdysseyWifi::busy\(\)/);
  assert.match(ota,/odysseyRecording\.load\(\) \|\| OdysseyWifi::busy\(\)/);
});

test('Wi-Fi failure leaves SD source intact and powers radio down',()=>{
  assert.match(wifi,/SD original kept/);
  assert.match(wifi,/WiFi\.disconnect\(true,false\)/);
  assert.match(wifi,/WiFi\.mode\(WIFI_OFF\)/);
  assert.match(wifi,/batteryCritical\(\)/);
});
