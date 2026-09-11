// Compile the production protocol engine with a fake flash backend.
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('native firmware engine rejects mismatched targets and preserves resumable OTA across phone suspension',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const engine=source.slice(source.indexOf('static void put32le('),source.indexOf('#include <esp_ota_ops.h>'));
  const fixture=fs.readFileSync(path.join(__dirname,'session.cpp'),'utf8');
  assert.match(nativeTest('#include <cstdint>\n#include <cstddef>\n#include <cstring>\n'+engine+'\n'+fixture),/PASS/);
});
