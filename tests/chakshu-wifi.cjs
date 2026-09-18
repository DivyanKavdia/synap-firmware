'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const source=fs.readFileSync('firmware/xiao-sense/wifi-downloads.cpp','utf8');

test('Chakshu private download portal keeps its contract without Arduino WebServer',()=>{
  assert.match(source,/#include <WiFi\.h>/);
  assert.doesNotMatch(source,/#include <WebServer\.h>|\bWebServer\b/);
  assert.match(source,/WiFiServer\* server=nullptr/);
  assert.match(source,/server=new\(std::nothrow\) WiFiServer\(80\)/);
  assert.match(source,/WiFiClient client=server->available\(\)/);
  assert.match(source,/method=="GET" && \(target=="\/"\|\|target\.startsWith\("\/\?"\)\)/);
  assert.match(source,/method=="GET" && target\.startsWith\("\/file\?"\)/);
  assert.match(source,/method=="POST" && target\.startsWith\("\/stop\?"\)/);
  assert.match(source,/queryValue\(target,"key"\)!=current\.token/);
  assert.match(source,/const String path=queryValue\(target,"path"\)/);
  assert.match(source,/if\(!validPath\(path\)\)/);
  assert.match(source,/Cache-Control: no-store/);
  assert.match(source,/Referrer-Policy: no-referrer/);
  assert.match(source,/X-Content-Type-Options: nosniff/);
  assert.match(source,/Content-Security-Policy:/);
  assert.match(source,/while\(sent<size && client\.connected\(\) && !stopRequested\.load\(\) && millis\(\)-started<MAX_MS\)/);
  assert.match(source,/IDLE_MS=180000,MAX_MS=900000/);
  assert.match(source,/http:\/\/192\.168\.4\.1\/\?key=%s/);
});

test('download paths remain restricted to Synap capture files',()=>{
  const body=source.slice(source.indexOf('bool validPath('),source.indexOf('String queryValue('));
  assert.match(body,/!path\.startsWith\("\/synap\/"\)/);
  assert.match(body,/path\[15\]!='-'/);
  assert.match(body,/path\[24\]!='\.'/);
  for(const ext of ['jpg','mjpeg','wav','json'])assert.match(body,new RegExp('extension=="'+ext+'"'));
  assert.doesNotMatch(body,/txt|html|bin/);
});

test('HTTP parser is bounded and closes every accepted client',()=>{
  assert.match(source,/out\.length\(\)>=511/);
  assert.match(source,/millis\(\)-began<1500u/);
  assert.match(source,/for\(unsigned n=0;n<32;\+\+n\)/);
  assert.match(source,/if\(client\)\{handle\(client\);client\.stop\(\);\}/);
  assert.match(source,/Connection: close/);
});
