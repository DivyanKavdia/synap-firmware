'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const src=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
test('SD catalogue never silently skips a failed FAT stat',()=>{
  const c=src.split('static uint8_t catalogue(uint32_t& total) {')[1].split('static uint8_t removeFailureStep=0;')[0];
  assert.match(c,/catalogueErrno=errno\?errno:EIO;catalogueStep=4;break/);
  assert.match(c,/catalogueErrno=EOVERFLOW;catalogueStep=6;break/);
  assert.match(c,/if \(catalogueErrno\)/);
});
test('C3 distinguishes FAT unlink errors and preserves failed file',()=>{
  const d=src.split('static uint8_t removeFile(const char* path) {')[1].split('static uint16_t clearRecordings()')[0];
  for(const step of [1,2,3])assert.match(d,new RegExp('removeFailureStep='+step+';return IO_ERROR'));
  assert.match(src,/noteSdMediaFault\(17,removeFailureStep,removeErr\)/);
  assert.match(src,/noteSdMediaFault\(7,catalogueStep,catalogueErrno\)/);
  assert.match(src,/catalogueErrno!=ENOMEM && catalogueErrno!=EOVERFLOW/);
});
test('new SD media I/O fault kept separate from older offline recorder error',()=>{
  for(const field of ['ioOp','ioStep','ioErr','faultKind','recordStage'])assert(src.includes(field));
  const sketch=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
  assert(sketch.includes('noteSdMediaFault(17,removeFailureStep,removeErr)'));
});
