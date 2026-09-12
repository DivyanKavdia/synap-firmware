'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
test('bounded recovery uses the production ring and negotiated ownership; unsafe resumes never stream',()=>{
  const code=source.slice(source.indexOf('// Optional recovery protocol.'),source.indexOf('void stopStreaming(ErrorCode reason) {'));
  assert.match(nativeTest(fs.readFileSync(path.join(__dirname,'recovery-runtime.cpp'),'utf8').replace('// INSERT RECOVERY',code)),/PASS recovery/);
});
