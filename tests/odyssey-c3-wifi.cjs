'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..');
const wifi=fs.readFileSync(path.join(root,'firmware/shared/odyssey-c3-wifi.cpp'),'utf8');
test('diagnostic branch force-links C3 Wi-Fi STA without TLS or HTTP',()=>{
  assert.match(wifi,/#include <WiFi\.h>/);
  assert.match(wifi,/WiFi\.mode\(WIFI_STA\)/);
  assert.match(wifi,/WiFi\.begin\("SYNAP-SIZE-PROBE"/);
  assert.doesNotMatch(wifi,/WiFiClientSecure|HTTPClient|setCACert|setInsecure/);
});
