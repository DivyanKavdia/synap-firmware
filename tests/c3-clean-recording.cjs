'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

test('clean recorder saves exact PCM and preserves partials on write/sync/close failure',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 const header=source.slice(source.indexOf('static void odysseyCleanWavHeader'),source.indexOf('static void odysseyCleanPrepareHost'));
 const recorder=source.slice(source.indexOf('static bool odysseyCleanWriteAll'),source.indexOf('namespace OdysseyTransfer'));
 const body=fs.readFileSync('tests/c3-clean-recording.cpp','utf8')
   .replace('// INSERT PRODUCTION',(header+recorder).replaceAll('/odyssey-sd/synap/',''));
 nativeTest(body,['-DUSE_REAL_I2S_MIC=1']);
});

test('clean recorder keeps card commands in the SD library and writes at most one sector per call',()=>{
 const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');
 assert.match(source,/ODYSSEY_SD_SPI_HZ=1000000u/);
 assert.match(source,/ODYSSEY_SD_WRITE_CHUNK_BYTES=512u/);
 assert.match(source,/chunk=ODYSSEY_SD_WRITE_CHUNK_BYTES/);
 assert.match(source,/One sector per VFS write keeps normal audio off CMD25 multi-block writes/);
 assert.doesNotMatch(source,/odysseyCleanRawByte|odysseyCleanCmd0|odysseyCleanStopOldTransfer|odysseyCleanResyncBeforeMount/);
 assert.doesNotMatch(source,/0xFD|CMD12|CMD25 stop/);
});
